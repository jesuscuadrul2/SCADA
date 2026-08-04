# 🏭 Nodo SCADA Edge-IoT basado en ESP32 y Arquitectura de Microservicios

![C++](https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![Python](https://img.shields.io/badge/python-3670A0?style=for-the-badge&logo=python&logoColor=ffdd54)
![Docker](https://img.shields.io/badge/docker-%230db7ed.svg?style=for-the-badge&logo=docker&logoColor=white)
![MQTT](https://img.shields.io/badge/mqtt-660066?style=for-the-badge&logo=mqtt&logoColor=white)
![InfluxDB](https://img.shields.io/badge/InfluxDB-22ADF6?style=for-the-badge&logo=InfluxDB&logoColor=white)
![Grafana](https://img.shields.io/badge/grafana-%23F46800.svg?style=for-the-badge&logo=grafana&logoColor=white)

Diseño e implementación integral de un nodo de Control Supervisorio y Adquisición de Datos (SCADA) para entornos industriales simulados. El sistema integra telemetría de potencia en tiempo real, control de acceso basado en roles mediante NFC, mitigación de fallos físicos y un backend contenerizado para almacenamiento y visualización.

## ✨ Características Principales

* **🛡️ Control de Acceso Basado en Roles (RBAC):** Autenticación NFC (PN532) validada en tiempo real contra una base de datos remota MariaDB vía MQTT. Roles definidos: `OPERADOR`, `TÉCNICO`, `GERENTE`.
* **⚡ Telemetría de Potencia en Tiempo Real:** Monitorización de voltaje, corriente (mA) y potencia (mW) utilizando sensores I2C de alta precisión (INA219), muestreados a ~3Hz.
* **🛑 Sistema de Paro de Emergencia y Protecciones:**
    * Mitigación de corrientes de irrupción (*Inrush current*) durante los primeros 4 segundos de encendido de cargas inductivas/capacitivas.
    * Disparo automático por sobrecorriente (Umbrales configurados a >400mA en L1 y >100mA en L2).
    * Monitorización de fallos en planta (Compuerta abierta, falta de material, desconexión del servidor).
* **💻 Sistema Operativo Embebido (SCADA OS 3.0):** Interfaz gráfica circular HMI desarrollada con la librería Adafruit GFX sobre una pantalla TFT GC9A01A (SPI), controlada mediante un *rotary encoder*.
* **☁️ Backend Edge/Local:** Ecosistema completo desplegado mediante `docker-compose` con bases de datos relacionales (MariaDB) para seguridad, bases de datos de series temporales (InfluxDB) para telemetría, broker de mensajería (Mosquitto) y visualización (Grafana).

## 📐 Arquitectura del Sistema

### 1. Hardware (Frontend IoT / Edge Node)
* **Microcontrolador:** ESP32
* **HMI Visual:** Pantalla TFT Redonda 1.28" (Controlador GC9A01A - Interfaz SPI)
* **Sensores de Potencia:** 2x INA219 (Interfaz I2C) para medición de V, mA y mW.
* **Control de Acceso:** Módulo RFID/NFC PN532 (Interfaz I2C)
* **Actuadores e Indicadores:** Módulo de 4 Relés Optoacoplados, Buzzer Activo, NeoPixel WS2812B.

### 2. Middleware (SCADA Brain)
El núcleo lógico del sistema está desarrollado en **Python 3**. Funciona como un servicio en segundo plano que escucha el tráfico de la red y orquesta las bases de datos:
* **Lógica de Seguridad (`mysql.connector`):** Valida los UID de las tarjetas NFC entrantes, asigna roles y gestiona altas/bajas de usuarios.
* **Auditoría y Logs:** Registra cada intento de acceso, disparo por sobrecorriente o fallo físico en la planta.
* **Ingesta de Telemetría (`influxdb-client`):** Convierte los payloads JSON de la ESP32 en *Points* de series temporales y los inyecta sincrónicamente en el *bucket* de InfluxDB.

### 3. Software (Backend Dockerizado)
El ecosistema completo está desplegado mediante `docker-compose` para garantizar portabilidad y alta disponibilidad en servidores locales.
* `db_scada_auth` (MariaDB 10.11): Gestión de credenciales y registro de eventos.
* `adminer_SCADA` (Adminer): Interfaz web ligera para la gestión directa de MariaDB.
* `mosquitto_scada` (Eclipse Mosquitto): Broker MQTT central para la mensajería asíncrona M2M.
* `influxdb_scada` (InfluxDB 2.7): Base de datos TSDB optimizada para la alta frecuencia de escritura de la telemetría eléctrica.
* `grafana_scada` (Grafana OSS): Dashboards de visualización y analítica en tiempo real.

## 📡 Flujo de Datos y Protocolo (MQTT Topics)

El sistema utiliza un bus de eventos MQTT acoplado al script `SCADA.Brain.py` para mantener la separación de responsabilidades:

| Tópico | Dirección | Descripción | Payload |
| :--- | :--- | :--- | :--- |
| `jarvis/scada/telemetry` | ESP32 -> Brain -> InfluxDB | Datos eléctricos de cargas | JSON `{v1, ma1, mw1...}` |
| `jarvis/scada/auth/request` | ESP32 -> Brain -> MariaDB | Petición de validación NFC | JSON `{uid}` |
| `jarvis/scada/auth/response`| MariaDB -> Brain -> ESP32 | Respuesta de autorización | JSON `{uid, rol, nombre}` |
| `jarvis/scada/admin/add` | ESP32 -> Brain -> MariaDB | Alta/Actualización de Tag | JSON `{uid, rol}` |
| `jarvis/scada/admin/remove` | ESP32 -> Brain -> MariaDB | Revocación de Tag | JSON `{uid}` |
| `jarvis/scada/log` | ESP32 -> Brain -> MariaDB | Registro de alarmas y eventos | JSON `{uid, evento, nivel}` |

## Instalación y Despliegue

### 1. Despliegue del Backend
Asegúrate de tener instalado Docker y Docker Compose en tu servidor local.

bash
# Clonar el repositorio
git clone [https://github.com/jesuscuadrul2/SCADA.git](https://github.com/jesuscuadrul2/SCADA.git)
cd SCADA

2. Configuración del Middleware Python
Instala las dependencias necesarias para ejecutar el cerebro del SCADA:

Bash
pip install paho-mqtt mysql-connector-python influxdb-client
python SCADA.Brain.py

3. Configuración del Firmware
Abre el archivo principal en PlatformIO o Arduino IDE.

Modifica las credenciales de red y el servidor MQTT en la sección de configuración:

C++
const char* ssid = "TU_RED_WIFI";
const char* password = "TU_PASSWORD";
const char* mqtt_server = "IP_DE_TU_SERVIDOR";

# Levantar los microservicios
docker-compose up -d

⚙️ Máquina de Estados HMI (ESP32)
El ESP32 opera mediante una máquina de estados finitos no bloqueante (millis()):

SALVAPANTALLAS: Reloj en espera (Standby - Locked).

TELEMETRIA: Visualización de potencia por canal en tiempo real.

CONTROL_CARGAS: Activación de relés (Acceso restringido a Técnico/Gerente).

ALERTA_INTRUSO: Bloqueo por tag no registrado con alarma visual/sonora.

CONFIRMAR_OVERRIDE: Desbloqueo tras un paro de emergencia (Exclusivo para Gerente).

MENU_ADMIN: Gestión de usuarios NFC (Añadir/Eliminar).
