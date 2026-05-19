import paho.mqtt.client as mqtt
import mysql.connector
import json
import time
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

# --- CONFIGURACIÓN ---
MQTT_BROKER = "127.0.0.1" 
DB_HOST = "127.0.0.1"
DB_USER = "root"
DB_PASS = "(JC2)^-1"
DB_NAME = "seguridad_scada"
DB_PORT = 3307 

# Configuración InfluxDB
INFLUX_URL = "http://127.0.0.1:8086"
INFLUX_TOKEN = "ingeniero_cucei_token_pro"
INFLUX_ORG = "mi_fabrica"
INFLUX_BUCKET = "ina219_data"

# Conexión a MariaDB
def get_db_connection():
    return mysql.connector.connect(
        host=DB_HOST, 
        user=DB_USER, 
        password=DB_PASS, 
        database=DB_NAME,
        port=DB_PORT 
    )

# Cliente InfluxDB
influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)

# --- CALLBACKS MQTT ---
def on_connect(client, userdata, flags, rc):
    print("✅ Conectado al Broker MQTT. Escuchando a la ESP32...")
    client.subscribe("jarvis/scada/auth/request")
    client.subscribe("jarvis/scada/admin/add")
    client.subscribe("jarvis/scada/admin/remove")
    client.subscribe("jarvis/scada/log")
    client.subscribe("jarvis/scada/telemetry") # Nuevo tópico

def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode('utf-8'))
    except Exception as e:
        return 

    # --- 1 a 4. LÓGICA DE SEGURIDAD (MARIADB) ---
    if topic in ["jarvis/scada/auth/request", "jarvis/scada/admin/add", "jarvis/scada/admin/remove", "jarvis/scada/log"]:
        db = get_db_connection()
        cursor = db.cursor(dictionary=True)

        if topic == "jarvis/scada/auth/request":
            uid = payload.get("uid")
            cursor.execute("SELECT rol, nombre FROM usuarios WHERE uid = %s", (uid,))
            user = cursor.fetchone()
            
            rol = user["rol"] if user else 0 
            nombre = user["nombre"] if user else "Desconocido"
            
            respuesta = {"uid": uid, "rol": rol, "nombre": nombre}
            client.publish("jarvis/scada/auth/response", json.dumps(respuesta))
            print(f"🔐 Auth Request: {uid} -> Rol: {rol} ({nombre})")

        elif topic == "jarvis/scada/admin/add":
            uid = payload.get("uid")
            rol = payload.get("rol")
            nombres_roles = {1: "Operador", 2: "Tecnico", 3: "Gerente"}
            nombre_asignado = f"Nuevo {nombres_roles.get(rol, 'Empleado')}"

            cursor.execute("""
                INSERT INTO usuarios (uid, rol, nombre) 
                VALUES (%s, %s, %s) 
                ON DUPLICATE KEY UPDATE rol = %s, nombre = %s
            """, (uid, rol, nombre_asignado, rol, nombre_asignado))
            db.commit()
            print(f"📝 TAG Registrado: {uid} con Rol: {rol} ({nombre_asignado})")

        elif topic == "jarvis/scada/admin/remove":
            uid = payload.get("uid")
            cursor.execute("UPDATE usuarios SET rol = 0 WHERE uid = %s", (uid,))
            db.commit()
            print(f"🗑️ TAG Revocado: {uid}")

        elif topic == "jarvis/scada/log":
            uid_activo = payload.get("uid", "SISTEMA")
            evento = payload.get("evento")
            nivel = payload.get("nivel", 1)
            
            cursor.execute("INSERT INTO logs_eventos (uid_activo, evento, nivel_alerta) VALUES (%s, %s, %s)", 
                           (uid_activo, evento, nivel))
            db.commit()
            print(f"📋 LOG: [{uid_activo}] -> {evento}")

        cursor.close()
        db.close()

    # --- 5. LÓGICA DE TELEMETRÍA (INFLUXDB) ---
    elif topic == "jarvis/scada/telemetry":
        try:
            for i in range(1, 3):
                p = Point("telemetria_cargas") \
                    .tag("dispositivo", "ESP32_S3") \
                    .tag("id_carga", f"carga_{i}") \
                    .field("voltaje", float(payload[f"v{i}"])) \
                    .field("corriente", float(payload[f"ma{i}"])) \
                    .field("potencia", float(payload[f"mw{i}"]))
                write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=p)
        except Exception as e:
            print(f"❌ Error en telemetría: {e}")

# --- INICIO DEL SERVICIO ---
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
client.on_connect = on_connect
client.on_message = on_message

try:
    client.connect(MQTT_BROKER, 1884, 60)
    client.loop_forever()
except KeyboardInterrupt:
    print("Servicio Brain detenido.")
