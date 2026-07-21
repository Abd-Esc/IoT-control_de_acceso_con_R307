#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>
#include <ESP32Servo.h>

#include "secrets.h"

// =====================================================
// 1. CONFIGURACIÓN WIFI Y BACKEND
// =====================================================

// Estos valores están definidos en secrets.h:
//
// WIFI_SSID
// WIFI_PASSWORD
// API_BASE_URL
// DEVICE_API_KEY

// IMPORTANTE:
// El backend NO cambia.
//
// Aunque ahora usamos huella, seguimos enviando:
//
// POST /acceso/nfc-scan
// {
//   "nfc_uid": "1"
// }
//
// donde "1" es el ID interno encontrado por el R307.

static const char* ACCESS_ENDPOINT = "/acceso/nfc-scan";

static const char* WIFI_HOSTNAME = "esp32GYM";

// =====================================================
// 2. PINES DEL SISTEMA
// =====================================================

// -----------------------------------------------------
// R307
// -----------------------------------------------------
//
// R307 TX -> ESP32 GPIO16
// R307 RX -> ESP32 GPIO17
//
// Los nombres RX/TX se observan desde el ESP32.

static const uint8_t R307_RX_PIN = 16;
static const uint8_t R307_TX_PIN = 17;

// -----------------------------------------------------
// Servo MG995
// -----------------------------------------------------

static const uint8_t SERVO_PIN = 21;

// -----------------------------------------------------
// LED acceso autorizado
// -----------------------------------------------------

static const uint8_t LED_GREEN_PIN = 26;

// -----------------------------------------------------
// PIR HC-SR501
// -----------------------------------------------------

static const uint8_t PIR_PIN = 25;

// -----------------------------------------------------
// LED encendido mientras PIR detecta movimiento
// -----------------------------------------------------

static const uint8_t LED_PIR_PIN = 32;

// -----------------------------------------------------
// LED de error de red
// -----------------------------------------------------

static const uint8_t LED_NETWORK_ERROR_PIN = 22;

// -----------------------------------------------------
// Buzzer activo para acceso denegado
// -----------------------------------------------------

static const uint8_t BUZZER_PIN = 33;

// =====================================================
// 3. CONFIGURACIÓN R307
// =====================================================

// Baudrate habitual del R307.
static const uint32_t R307_BAUD = 57600;

// UART2 del ESP32.
HardwareSerial r307Serial(2);

// Objeto de la librería Adafruit.
Adafruit_Fingerprint finger(&r307Serial);

// =====================================================
// 4. CONFIGURACIÓN DEL SERVO
// =====================================================

Servo servoAcceso;

static const int SERVO_CERRADO_GRADOS = 90;
static const int SERVO_ABIERTO_GRADOS = 180;

static const int SERVO_PULSO_MIN_US = 500;
static const int SERVO_PULSO_MAX_US = 2400;

// =====================================================
// 5. CONFIGURACIÓN DE TIEMPOS
// =====================================================

// Si se abre el acceso pero el PIR nunca detecta una persona,
// cerrar después de 10 segundos.
static const unsigned long TIEMPO_MAX_APERTURA_SIN_PIR_MS = 10000;

// Una vez que el PIR deja de detectar movimiento,
// esperar 1 segundo antes de cerrar.
static const unsigned long TIEMPO_ESPERA_CIERRE_TRAS_PIR_LOW_MS = 1000;

// Intervalo entre consultas al lector de huellas.
static const unsigned long INTERVALO_LECTURA_HUELLA_MS = 250;

// Tiempo inicial de estabilización del PIR.
static const unsigned long TIEMPO_ESTABILIZACION_PIR_MS = 30000;

// Duración del buzzer cuando se deniega acceso.
static const unsigned long DURACION_BUZZER_DENEGADO_MS = 500;

// Reintentos.
static const unsigned long INTERVALO_REINTENTO_WIFI_MS = 5000;
static const unsigned long INTERVALO_REINTENTO_R307_MS = 5000;

// Timeout HTTP.
// En el código NFC original estaba definido,
// aunque http.setTimeout() estaba comentado.
static const unsigned long HTTP_TIMEOUT_MS = 3000;

// =====================================================
// 6. ESTADOS DEL SISTEMA DE ACCESO
// =====================================================

enum class EstadoAcceso {

  // Puerta cerrada.
  CERRADO,

  // Se autorizó acceso y se abrió.
  // Estamos esperando que el PIR detecte a la persona.
  ABIERTO_ESPERANDO_PIR,

  // El PIR detectó a la persona.
  ABIERTO_CON_USUARIO_DETECTADO,

  // La persona dejó de ser detectada.
  // Esperamos un momento antes de cerrar.
  ABIERTO_ESPERANDO_CIERRE

};

// =====================================================
// 7. RESPUESTA DEL BACKEND
// =====================================================

struct RespuestaBackend {

  bool comunicacionOk;

  bool accesoConcedido;

  int httpCode;

  String mensaje;

  String motivoDenegacion;
};

EstadoAcceso estadoActual = EstadoAcceso::CERRADO;

// =====================================================
// 8. VARIABLES DE CONTROL
// =====================================================

unsigned long tiempoInicioSistema = 0;

unsigned long tiempoApertura = 0;

unsigned long tiempoInicioPIRLow = 0;

// -----------------------------------------------------
// Huella
// -----------------------------------------------------

unsigned long ultimaLecturaHuella = 0;

unsigned long ultimoIntentoR307 = 0;

bool r307Disponible = false;

// Después de procesar una huella,
// obligamos al usuario a retirar el dedo.
//
// Esto evita:
//
// dedo mantenido
//   ↓
// POST
// POST
// POST
// POST...
//
// Solo habrá una lectura por colocación del dedo.
bool esperandoRetiroDedo = false;

// Cuenta errores UART consecutivos.
uint8_t erroresComunicacionR307 = 0;

// -----------------------------------------------------
// Wi-Fi
// -----------------------------------------------------

unsigned long ultimoIntentoWifi = 0;

bool wifiEstabaConectado = false;

bool falloRedActivo = true;

// -----------------------------------------------------
// Buzzer
// -----------------------------------------------------

unsigned long tiempoInicioBuzzer = 0;

bool buzzerActivo = false;

// -----------------------------------------------------
// Servo
// -----------------------------------------------------

int anguloServoActual = -1;

// =====================================================
// 9. UTILIDADES DE SALIDA
// =====================================================

void moverServoSiEsNecesario(int nuevoAngulo) {

  if (anguloServoActual == nuevoAngulo) {
    return;
  }

  servoAcceso.write(nuevoAngulo);

  anguloServoActual = nuevoAngulo;

  Serial.print("[SERVO] Angulo: ");
  Serial.println(nuevoAngulo);
}

// -----------------------------------------------------

void encenderLedVerde(bool encendido) {

  digitalWrite(
    LED_GREEN_PIN,
    encendido ? HIGH : LOW
  );
}

// -----------------------------------------------------

void encenderLedPIR(bool encendido) {

  digitalWrite(
    LED_PIR_PIN,
    encendido ? HIGH : LOW
  );
}

// -----------------------------------------------------

void encenderLedFalloRed(bool encendido) {

  digitalWrite(
    LED_NETWORK_ERROR_PIN,
    encendido ? HIGH : LOW
  );
}

// -----------------------------------------------------

void marcarEstadoRed(bool disponible) {

  falloRedActivo = !disponible;

  encenderLedFalloRed(falloRedActivo);
}

// -----------------------------------------------------

void iniciarBuzzerDenegado() {

  if (buzzerActivo) {
    return;
  }

  digitalWrite(BUZZER_PIN, HIGH);

  buzzerActivo = true;

  tiempoInicioBuzzer = millis();
}

// -----------------------------------------------------

void actualizarBuzzer() {

  if (!buzzerActivo) {
    return;
  }

  if (
    millis() - tiempoInicioBuzzer
    >= DURACION_BUZZER_DENEGADO_MS
  ) {

    digitalWrite(BUZZER_PIN, LOW);

    buzzerActivo = false;
  }
}

// =====================================================
// 10. PIR
// =====================================================

bool pirEstaEstabilizado() {

  return (
    millis() - tiempoInicioSistema
    >= TIEMPO_ESTABILIZACION_PIR_MS
  );
}

// -----------------------------------------------------

bool leerPIR() {

  if (!pirEstaEstabilizado()) {

    return false;
  }

  return digitalRead(PIR_PIN) == HIGH;
}

// =====================================================
// 11. WIFI
// =====================================================

void iniciarWiFi() {

  marcarEstadoRed(false);

  Serial.println("[WIFI] Iniciando");

  Serial.print("[WIFI] SSID: ");
  Serial.println(WIFI_SSID);

  // Evita guardar credenciales repetidamente
  // en memoria flash.
  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);

  WiFi.setHostname(WIFI_HOSTNAME);

  WiFi.setAutoReconnect(true);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  wifiEstabaConectado = false;

  ultimoIntentoWifi = millis();
}

// -----------------------------------------------------

void actualizarWiFi() {

  bool wifiConectado =
    WiFi.status() == WL_CONNECTED;

  if (wifiConectado) {

    if (!wifiEstabaConectado) {

      wifiEstabaConectado = true;

      marcarEstadoRed(true);

      Serial.print("[WIFI] Conectado. IP: ");

      Serial.println(
        WiFi.localIP()
      );
    }

    return;
  }

  // ---------------------------------------------------

  if (wifiEstabaConectado) {

    wifiEstabaConectado = false;

    marcarEstadoRed(false);

    Serial.println(
      "[WIFI] Conexion perdida."
    );
  }

  // ---------------------------------------------------

  unsigned long ahora = millis();

  if (
    ahora - ultimoIntentoWifi
    < INTERVALO_REINTENTO_WIFI_MS
  ) {

    return;
  }

  WiFi.reconnect();

  ultimoIntentoWifi = ahora;
}

// -----------------------------------------------------

bool wifiDisponible() {

  return WiFi.isConnected();
}

// =====================================================
// 12. R307 - INICIALIZACIÓN
// =====================================================

bool inicializarR307() {

  Serial.println();
  Serial.println("[R307] Inicializando lector de huellas.");

  // Reiniciamos UART2 por seguridad,
  // especialmente útil en los reintentos.
  r307Serial.end();

  delay(50);

  r307Serial.begin(
    R307_BAUD,
    SERIAL_8N1,
    R307_RX_PIN,
    R307_TX_PIN
  );

  delay(500);

  // ---------------------------------------------------
  // Verificar comunicación
  // ---------------------------------------------------

  if (!finger.verifyPassword()) {

    Serial.println(
      "[R307] No responde."
    );

    Serial.println(
      "[R307] Se reintentara sin bloquear el sistema."
    );

    return false;
  }

  Serial.println(
    "[R307] Comunicacion establecida."
  );

  // ---------------------------------------------------
  // Leer parámetros
  // ---------------------------------------------------

  uint8_t resultado =
    finger.getParameters();

  if (resultado == FINGERPRINT_OK) {

    Serial.print(
      "[R307] Capacidad maxima: "
    );

    Serial.println(
      finger.capacity
    );

    Serial.print(
      "[R307] Nivel seguridad: "
    );

    Serial.println(
      finger.security_level
    );
  }

  // ---------------------------------------------------
  // Saber cuántas huellas existen
  // ---------------------------------------------------

  resultado =
    finger.getTemplateCount();

  if (resultado == FINGERPRINT_OK) {

    Serial.print(
      "[R307] Huellas almacenadas: "
    );

    Serial.println(
      finger.templateCount
    );
  }

  erroresComunicacionR307 = 0;

  esperandoRetiroDedo = false;

  Serial.println(
    "[R307] Listo para identificar huellas."
  );

  return true;
}

// =====================================================
// 13. R307 - REINTENTO AUTOMÁTICO
// =====================================================

void actualizarR307() {

  if (r307Disponible) {

    return;
  }

  unsigned long ahora = millis();

  if (
    ahora - ultimoIntentoR307
    < INTERVALO_REINTENTO_R307_MS
  ) {

    return;
  }

  ultimoIntentoR307 = ahora;

  r307Disponible =
    inicializarR307();
}

// =====================================================
// 14. CONTROL DE ERRORES DEL R307
// =====================================================

void registrarErrorComunicacionR307() {

  erroresComunicacionR307++;

  Serial.print(
    "[R307] Error UART consecutivo: "
  );

  Serial.println(
    erroresComunicacionR307
  );

  // No declaramos desconectado al sensor
  // por un único error aislado.
  if (erroresComunicacionR307 < 3) {

    return;
  }

  Serial.println(
    "[R307] Se perdio comunicacion con el sensor."
  );

  r307Disponible = false;

  erroresComunicacionR307 = 0;

  ultimoIntentoR307 = millis();
}

// -----------------------------------------------------

void limpiarErroresR307() {

  erroresComunicacionR307 = 0;
}

// =====================================================
// 15. BACKEND
// =====================================================

RespuestaBackend consultarBackendPorHuella(
  uint16_t huellaId
) {

  RespuestaBackend respuesta;

  respuesta.comunicacionOk = false;

  respuesta.accesoConcedido = false;

  respuesta.httpCode = -1;

  respuesta.mensaje =
    "Sin respuesta del backend";

  respuesta.motivoDenegacion =
    "Error de comunicacion";

  // ---------------------------------------------------
  // Wi-Fi
  // ---------------------------------------------------

  if (!wifiDisponible()) {

    marcarEstadoRed(false);

    respuesta.mensaje =
      "WiFi no conectado";

    return respuesta;
  }

  // ---------------------------------------------------
  // Crear cliente HTTP
  // ---------------------------------------------------

  WiFiClient client;

  HTTPClient http;

  String url =
    String(API_BASE_URL)
    + String(ACCESS_ENDPOINT);

  if (!http.begin(client, url)) {

    marcarEstadoRed(false);

    respuesta.mensaje =
      "No se pudo iniciar HTTP";

    return respuesta;
  }

  // En el código NFC anterior estaba comentado.
  // Puedes activarlo si deseas limitar las esperas:
  //
  // http.setTimeout(HTTP_TIMEOUT_MS);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  // API key opcional.
  if (
    strlen(DEVICE_API_KEY) > 0
  ) {

    http.addHeader(
      "X-Device-Key",
      DEVICE_API_KEY
    );
  }

  // ===================================================
  // IMPORTANTE
  //
  // El backend sigue recibiendo "nfc_uid".
  //
  // Pero el valor ahora es el ID del R307.
  //
  // Ejemplo:
  //
  // Huella encontrada en ID 2
  //
  // {
  //   "nfc_uid": "2"
  // }
  //
  // ===================================================

  StaticJsonDocument<128> requestDoc;

  // Convertimos explícitamente el número a String.
  //
  // De esta forma obtenemos:
  //
  // "nfc_uid": "1"
  //
  // y NO:
  //
  // "nfc_uid": 1

  String identificadorHuella =
    String(huellaId);

  requestDoc["nfc_uid"] =
    identificadorHuella;

  String requestBody;

  requestBody.reserve(80);

  serializeJson(
    requestDoc,
    requestBody
  );

  // ---------------------------------------------------
  // Mostrar petición
  // ---------------------------------------------------

  Serial.println();

  Serial.print("[HTTP] POST ");

  Serial.println(url);

  Serial.print("[HTTP] Body: ");

  Serial.println(requestBody);

  // ---------------------------------------------------
  // Realizar POST
  // ---------------------------------------------------

  int httpCode =
    http.POST(requestBody);

  respuesta.httpCode =
    httpCode;

  if (httpCode <= 0) {

    marcarEstadoRed(false);

  } else {

    marcarEstadoRed(true);
  }

  // ---------------------------------------------------
  // Leer respuesta
  // ---------------------------------------------------

  String responseBody =
    http.getString();

  http.end();

  Serial.print(
    "[HTTP] Code: "
  );

  Serial.println(
    httpCode
  );

  Serial.print(
    "[HTTP] Response: "
  );

  Serial.println(
    responseBody
  );

  // ---------------------------------------------------
  // Verificar HTTP
  // ---------------------------------------------------

  if (
    httpCode < 200
    || httpCode >= 300
  ) {

    respuesta.mensaje =
      "Backend respondio error HTTP";

    return respuesta;
  }

  // ---------------------------------------------------
  // Parsear JSON
  // ---------------------------------------------------

  StaticJsonDocument<512>
    responseDoc;

  DeserializationError error =
    deserializeJson(
      responseDoc,
      responseBody
    );

  if (error) {

    respuesta.mensaje =
      "JSON invalido desde backend";

    return respuesta;
  }

  // ---------------------------------------------------
  // Extraer respuesta
  // ---------------------------------------------------

  respuesta.comunicacionOk = true;

  respuesta.accesoConcedido =
    responseDoc[
      "acceso_concedido"
    ] | false;

  respuesta.mensaje =
    responseDoc[
      "mensaje"
    ] | "";

  respuesta.motivoDenegacion =
    responseDoc[
      "motivo_denegacion"
    ] | "";

  return respuesta;
}

// =====================================================
// 16. ACCIONES DEL SISTEMA
// =====================================================

void abrirAcceso(
  const String& mensaje
) {

  Serial.print(
    "[ACCESO] Autorizado: "
  );

  Serial.println(
    mensaje
  );

  moverServoSiEsNecesario(
    SERVO_ABIERTO_GRADOS
  );

  encenderLedVerde(true);

  tiempoApertura = millis();

  estadoActual =
    EstadoAcceso::
      ABIERTO_ESPERANDO_PIR;
}

// -----------------------------------------------------

void cerrarAccesoPorError(
  const String& error
) {

  Serial.print(
    "[ERROR] "
  );

  Serial.println(
    error
  );

  moverServoSiEsNecesario(
    SERVO_CERRADO_GRADOS
  );

  encenderLedVerde(false);

  estadoActual =
    EstadoAcceso::CERRADO;
}

// -----------------------------------------------------

void denegarAcceso(
  const String& motivo
) {

  Serial.print(
    "[ACCESO] Denegado: "
  );

  Serial.println(
    motivo
  );

  moverServoSiEsNecesario(
    SERVO_CERRADO_GRADOS
  );

  encenderLedVerde(false);

  iniciarBuzzerDenegado();

  estadoActual =
    EstadoAcceso::CERRADO;
}

// -----------------------------------------------------

void cerrarAcceso(
  const char* motivo
) {

  Serial.print(
    "[ACCESO] Cerrando: "
  );

  Serial.println(
    motivo
  );

  moverServoSiEsNecesario(
    SERVO_CERRADO_GRADOS
  );

  encenderLedVerde(false);

  estadoActual =
    EstadoAcceso::CERRADO;
}

// =====================================================
// 17. IDENTIFICACIÓN DE HUELLA
// =====================================================

void leerHuellaSiCorresponde() {

  // Solo se leen huellas cuando la puerta
  // está cerrada.
  if (
    estadoActual
    != EstadoAcceso::CERRADO
  ) {

    return;
  }

  // El sensor debe estar disponible.
  if (!r307Disponible) {

    return;
  }

  unsigned long ahora =
    millis();

  // Limitar frecuencia de consultas.
  if (
    ahora - ultimaLecturaHuella
    < INTERVALO_LECTURA_HUELLA_MS
  ) {

    return;
  }

  ultimaLecturaHuella = ahora;

  // ===================================================
  // PASO 0
  // Esperar a que el usuario retire el dedo
  // ===================================================

  if (esperandoRetiroDedo) {

    uint8_t resultado =
      finger.getImage();

    // El dedo fue retirado.
    if (
      resultado
      == FINGERPRINT_NOFINGER
    ) {

      esperandoRetiroDedo = false;

      limpiarErroresR307();

      Serial.println(
        "[R307] Dedo retirado. Listo para nueva lectura."
      );
    }

    // Problema de comunicación.
    else if (
      resultado
      == FINGERPRINT_PACKETRECIEVEERR
    ) {

      registrarErrorComunicacionR307();
    }

    return;
  }

  // ===================================================
  // PASO 1
  // Capturar imagen
  // ===================================================

  uint8_t resultado =
    finger.getImage();

  switch (resultado) {

    case FINGERPRINT_NOFINGER:

      limpiarErroresR307();

      return;

    case FINGERPRINT_OK:

      limpiarErroresR307();

      Serial.println();

      Serial.println(
        "[R307] Dedo detectado."
      );

      break;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println(
        "[R307] Error de comunicacion al capturar imagen."
      );

      registrarErrorComunicacionR307();

      return;

    case FINGERPRINT_IMAGEFAIL:

      Serial.println(
        "[R307] Error al capturar imagen."
      );

      esperandoRetiroDedo = true;

      return;

    default:

      Serial.print(
        "[R307] Error desconocido getImage(): 0x"
      );

      Serial.println(
        resultado,
        HEX
      );

      esperandoRetiroDedo = true;

      return;
  }

  // A partir de aquí no queremos procesar
  // nuevamente este mismo dedo.
  esperandoRetiroDedo = true;

  // ===================================================
  // PASO 2
  // Convertir imagen en características
  // ===================================================

  resultado =
    finger.image2Tz(1);

  switch (resultado) {

    case FINGERPRINT_OK:

      Serial.println(
        "[R307] Caracteristicas extraidas."
      );

      break;

    case FINGERPRINT_IMAGEMESS:

      Serial.println(
        "[R307] Imagen poco clara."
      );

      return;

    case FINGERPRINT_FEATUREFAIL:

    case FINGERPRINT_INVALIDIMAGE:

      Serial.println(
        "[R307] No se pudieron extraer caracteristicas."
      );

      return;

    case FINGERPRINT_PACKETRECIEVEERR:

      Serial.println(
        "[R307] Error de comunicacion en image2Tz()."
      );

      registrarErrorComunicacionR307();

      return;

    default:

      Serial.print(
        "[R307] Error image2Tz(): 0x"
      );

      Serial.println(
        resultado,
        HEX
      );

      return;
  }

  // ===================================================
  // PASO 3
  // Buscar la huella en la memoria interna del R307
  // ===================================================

  resultado =
    finger.fingerSearch();

  // ---------------------------------------------------
  // Huella NO encontrada
  // ---------------------------------------------------

  if (
    resultado
    == FINGERPRINT_NOTFOUND
  ) {

    Serial.println(
      "[R307] Huella NO registrada."
    );

    // IMPORTANTE:
    //
    // En este caso el R307 NO tiene un ID que enviar
    // al backend.
    //
    // Por eso se deniega localmente.
    //
    // No enviamos "0" ni un ID inventado.

    denegarAcceso(
      "Huella no registrada en el lector"
    );

    return;
  }

  // ---------------------------------------------------
  // Error de comunicación
  // ---------------------------------------------------

  if (
    resultado
    == FINGERPRINT_PACKETRECIEVEERR
  ) {

    Serial.println(
      "[R307] Error de comunicacion durante la busqueda."
    );

    registrarErrorComunicacionR307();

    return;
  }

  // ---------------------------------------------------
  // Otro error
  // ---------------------------------------------------

  if (
    resultado
    != FINGERPRINT_OK
  ) {

    Serial.print(
      "[R307] Error fingerSearch(): 0x"
    );

    Serial.println(
      resultado,
      HEX
    );

    return;
  }

  // ===================================================
  // PASO 4
  // Huella encontrada
  // ===================================================

  limpiarErroresR307();

  uint16_t huellaId =
    finger.fingerID;

  uint16_t confianza =
    finger.confidence;

  Serial.println(
    "[R307] Huella reconocida."
  );

  Serial.print(
    "[R307] ID: "
  );

  Serial.println(
    huellaId
  );

  Serial.print(
    "[R307] Confianza: "
  );

  Serial.println(
    confianza
  );

  // ===================================================
  // PASO 5
  // Enviar el ID al backend
  // ===================================================

  Serial.print(
    "[BACKEND] Enviando ID de huella como nfc_uid: "
  );

  Serial.println(
    huellaId
  );

  RespuestaBackend respuesta =
    consultarBackendPorHuella(
      huellaId
    );

  // ===================================================
  // PASO 6
  // Procesar decisión del backend
  // ===================================================

  if (!respuesta.comunicacionOk) {

    cerrarAccesoPorError(
      respuesta.mensaje
    );

    return;
  }

  // ---------------------------------------------------
  // Autorizado
  // ---------------------------------------------------

  if (respuesta.accesoConcedido) {

    abrirAcceso(
      respuesta.mensaje
    );

    return;
  }

  // ---------------------------------------------------
  // Denegado
  // ---------------------------------------------------

  String motivo =
    respuesta.motivoDenegacion.length() > 0
      ? respuesta.motivoDenegacion
      : respuesta.mensaje;

  denegarAcceso(
    motivo
  );
}

// =====================================================
// 18. CONTROL POR PIR
// =====================================================

void actualizarEstadoAcceso() {

  bool pirActivo =
    leerPIR();

  encenderLedPIR(
    pirActivo
  );

  unsigned long ahora =
    millis();

  switch (estadoActual) {

    // =================================================
    // PUERTA CERRADA
    // =================================================

    case EstadoAcceso::CERRADO:

      moverServoSiEsNecesario(
        SERVO_CERRADO_GRADOS
      );

      encenderLedVerde(false);

      break;

    // =================================================
    // PUERTA ABIERTA
    // Esperando que pase una persona
    // =================================================

    case EstadoAcceso::
      ABIERTO_ESPERANDO_PIR:

      if (pirActivo) {

        Serial.println(
          "[PIR] Movimiento detectado."
        );

        estadoActual =
          EstadoAcceso::
            ABIERTO_CON_USUARIO_DETECTADO;

        break;
      }

      // Si nadie pasa dentro del tiempo máximo,
      // cerrar automáticamente.
      if (
        ahora - tiempoApertura
        >= TIEMPO_MAX_APERTURA_SIN_PIR_MS
      ) {

        cerrarAcceso(
          "timeout sin deteccion PIR"
        );
      }

      break;

    // =================================================
    // USUARIO DETECTADO
    // =================================================

    case EstadoAcceso::
      ABIERTO_CON_USUARIO_DETECTADO:

      if (pirActivo) {

        break;
      }

      Serial.println(
        "[PIR] Sin movimiento. Esperando cierre."
      );

      tiempoInicioPIRLow =
        ahora;

      estadoActual =
        EstadoAcceso::
          ABIERTO_ESPERANDO_CIERRE;

      break;

    // =================================================
    // ESPERANDO CIERRE
    // =================================================

    case EstadoAcceso::
      ABIERTO_ESPERANDO_CIERRE:

      // Si vuelve a detectar movimiento,
      // cancelar el cierre.
      if (pirActivo) {

        Serial.println(
          "[PIR] Movimiento nuevamente. Cierre cancelado."
        );

        estadoActual =
          EstadoAcceso::
            ABIERTO_CON_USUARIO_DETECTADO;

        break;
      }

      // Si el PIR permanece libre,
      // cerrar.
      if (
        ahora - tiempoInicioPIRLow
        >= TIEMPO_ESPERA_CIERRE_TRAS_PIR_LOW_MS
      ) {

        cerrarAcceso(
          "PIR libre durante 1 segundo"
        );
      }

      break;
  }
}

// =====================================================
// 19. SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  tiempoInicioSistema =
    millis();

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "ESP32 + R307 + PIR + Servo + Backend"
  );

  Serial.println(
    "============================================"
  );

  // ---------------------------------------------------
  // Pines
  // ---------------------------------------------------

  pinMode(
    LED_GREEN_PIN,
    OUTPUT
  );

  pinMode(
    LED_PIR_PIN,
    OUTPUT
  );

  pinMode(
    LED_NETWORK_ERROR_PIN,
    OUTPUT
  );

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  pinMode(
    PIR_PIN,
    INPUT_PULLDOWN
  );

  // ---------------------------------------------------
  // Estado inicial de salidas
  // ---------------------------------------------------

  encenderLedVerde(false);

  encenderLedPIR(false);

  encenderLedFalloRed(true);

  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  // ---------------------------------------------------
  // Wi-Fi
  // ---------------------------------------------------

  iniciarWiFi();

  // ---------------------------------------------------
  // Servo
  // ---------------------------------------------------

  servoAcceso.setPeriodHertz(50);

  servoAcceso.attach(
    SERVO_PIN,
    SERVO_PULSO_MIN_US,
    SERVO_PULSO_MAX_US
  );

  moverServoSiEsNecesario(
    SERVO_CERRADO_GRADOS
  );

  // ---------------------------------------------------
  // R307
  // ---------------------------------------------------

  // Fuerza un intento inmediato.
  ultimoIntentoR307 =
    millis()
    - INTERVALO_REINTENTO_R307_MS;

  actualizarR307();

  // ---------------------------------------------------
  // Mensajes
  // ---------------------------------------------------

  Serial.println();

  Serial.println(
    "[SISTEMA] Inicializado."
  );

  Serial.println(
    "[SISTEMA] El R307 identifica la huella."
  );

  Serial.println(
    "[SISTEMA] El backend decide si autoriza el acceso."
  );

  Serial.println(
    "[BACKEND] Campo enviado: nfc_uid"
  );

  Serial.println(
    "[PIR] Ignorando lecturas iniciales por estabilizacion."
  );

  Serial.println();
}

// =====================================================
// 20. LOOP
// =====================================================

void loop() {

  // Mantener conexión Wi-Fi.
  actualizarWiFi();

  // Reintentar R307 si está desconectado.
  actualizarR307();

  // Apagar buzzer cuando termine su tiempo.
  actualizarBuzzer();

  // Controlar servo mediante máquina de estados + PIR.
  actualizarEstadoAcceso();

  // Leer una huella únicamente cuando el acceso
  // está cerrado.
  leerHuellaSiCorresponde();
}