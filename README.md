# ESPHome X-28 MPX/H Connector

[![ESPHome](https://img.shields.io/badge/ESPHome-2024.6+-blue.svg)](https://esphome.io)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Componente externo de ESPHome que se conecta al bus MPX/MPXH de centrales de alarma
**X-28** (series N y 900X). Permite armar/desarmar, monitorear zonas, inyectar zonas
virtuales, y emular un teclado completo — todo desde Home Assistant.

> Este proyecto fue **vibe-coded** con [OpenCode](https://opencode.ai) + DeepSeek V4 Flash.

## Instalación

Agrega esto a tu archivo YAML — ESPHome descarga automáticamente el componente al compilar, sin necesidad de clonar manualmente:

```yaml
external_components:
  - source: github://abelbour/esphome-x28-mpx-h-connector@main
    components: [ x28_alarm ]
```

## Modelos Compatibles

- Panel de control de alarma (`alarm_control_panel`) con estados: ARMADO, DESARMADO,
  PENDIENTE, DISPARADO, ARMADO_HOGAR, ARMADO_TOTAL
- Monitoreo de zonas (1–32, según modelo) como sensores binarios
- Sensor binario de modo Estoy (Stay) / Me Voy (Away)
- Botones de pánico y fuego (pulsación corta y larga)
- Modo rastreador (sniffer) que muestra todos los paquetes del bus
- Inyección de zonas virtuales: sensores de Home Assistant aparecen como zonas
  nativas en la central X-28
- Servicios de alto nivel para programación (panel, zonas, códigos, RF) sin
  recordar códigos P
- Servicio `send_keys` para enviar cualquier secuencia de teclas (programación
  completa de la central)
- Código de instalador configurable (por defecto `467825`)
- Modelos compatibles: N4-MPXH, N8-MPXH, N8F-MPXH, N16-MPXH, N32-MPXH, N32F-MPXH,
  9002-MPX, 9003-MPX, 9004-MPX y detección automática (AUTO)
- Código de usuario configurable (PIN de 4–6 dígitos)
- Configuración de inversión de pines RX/TX para distintos circuitos de
  interfaz

## Hardware Requerido

Se necesita un circuito adaptador de nivel entre los pines GPIO del ESP (3.3 V)
y el bus MPX/MPXH de la alarma (8–12 V).

### Esquema de Referencia

```
                +12V (del bus de alarma)
                 │
                 ├─ R1 (10 kΩ) ──┬─ Pin RX (ESP)
                 │               │
Bus MPX ──┬──────┤               ├─ GND (ESP + común alarma)
          │      │               │
          │      └─ D1 (zener 3.3 V) ─ GND
          │
          └──────┬─ C1 (100 nF)
                 │
                 ├─ R2 (1 kΩ) ──┬─ Colector Q1 (2N2222)
                                │
                               Pin TX (ESP) ── R3 (1 kΩ) ── Base Q1
                                                │
                                               Emisor Q1 ── GND
```

### Componentes

| Componente | Valor         | Propósito                     |
|------------|---------------|-------------------------------|
| Q1         | 2N2222 o NPN similar | Driver de línea para TX |
| D1         | Zener 3.3 V   | Sujetador de tensión RX       |
| R1         | 10 kΩ         | Pull-up RX                    |
| R2         | 1 kΩ          | Carga de colector             |
| R3         | 1 kΩ          | Límite de corriente de base   |
| C1         | 100 nF        | Filtro de ruido RX (opcional) |

**Alternativa:** Se puede usar un optoacoplador (ej. PC817) para aislamiento
galvánico. Las opciones `invert_rx` / `invert_tx` permiten adaptarse a
cualquier topología.

### Pines del ESP

| Pin | Dirección | Función                     | Requisitos                          |
|-----|-----------|-----------------------------|-------------------------------------|
| RX  | Entrada   | Recepción de datos del bus  | Capaz de interrupción, flanco CHANGE |
| TX  | Salida    | Transmisión de datos al bus | GPIO estándar, push-pull            |

**ESP32:** Cualquier GPIO sirve (todos soportan interrupciones externas).
**ESP8266:** Solo GPIO 0, 2, 4, 5, 12, 13, 14, 15 (`digitalPinToInterrupt`).

## Instalación

1. Clona o copia este repositorio en tu directorio de configuración de ESPHome:

   ```bash
   # En el directorio donde tienes tu configuración de ESPHome
   git clone https://github.com/tu-usuario/x28_alarm.git
   # o copia manualmente la carpeta components/x28_alarm/
   ```

2. La estructura debe quedar así:

   ```
   config/
   ├── esphome/
   ├── components/
   │   └── x28_alarm/
   │       ├── __init__.py
   │       ├── alarm_control_panel.py
   │       ├── binary_sensor.py
   │       ├── button.py
   │       ├── text_sensor.py
   │       ├── x28.h
   │       └── x28.cpp
   └── alarma.yaml
   ```

3. Referencia el componente externo en tu YAML:

   ```yaml
   external_components:
     - source:
         type: local
         path: components
   ```

## Configuración

### Mínima

```yaml
x28_alarm:
  rx_pin: GPIO22
  tx_pin: GPIO23
  code: "282828"
```

### Completa

```yaml
x28_alarm:
  id: x28
  rx_pin:
    number: GPIO22
    inverted: true
  tx_pin:
    number: GPIO23
    inverted: true
  code: "282828"            # PIN de usuario (4-6 dígitos)
  installer_code: "467825"  # Código de instalador (6 dígitos, opcional)
  model: N8F-MPXH           # Opcional: AUTO, N4-MPXH, N8-MPXH, N8F-MPXH,
                            #          N16-MPXH, N32-MPXH, 9002-MPX, 9003-MPX, 9004-MPX
  debug: true
  sniffing:
    enabled: true
    throttle_ms: 1000       # Mínimo ms entre paquetes duplicados
  virtual_zones:
    - zone: 5
      sensor_id: binary_sensor.puerta_trasera
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
  zone_debounce_ms: 500     # Duración de zona activada antes de auto-limpiar
```

### Parámetros

| Parámetro              | Tipo      | Requerido | Por defecto | Descripción                                |
|------------------------|-----------|-----------|-------------|--------------------------------------------|
| `id`                   | string    | no        | —           | ID del componente para referencias         |
| `rx_pin`               | pin       | sí        | —           | Pin de entrada conectado al bus MPX        |
| `tx_pin`               | pin       | sí        | —           | Pin de salida que conduce el bus           |
| `code`                 | string    | sí        | —           | PIN de usuario (4–6 dígitos numéricos)     |
| `installer_code`       | string    | no        | `467825`    | Código de instalador (6 dígitos)          |
| `model`                | string    | no        | `AUTO`      | Modelo de central X-28                     |
| `invert_rx`            | boolean   | no        | `true`      | Invertir señal RX                          |
| `invert_tx`            | boolean   | no        | `true`      | Invertir señal TX                          |
| `debug`                | boolean   | no        | `false`     | Habilitar logs verbose (`ESP_LOGV`)        |
| `sniffing.enabled`     | boolean   | no        | `false`     | Habilitar rastreo de todos los paquetes    |
| `sniffing.throttle_ms` | int       | no        | `1000`      | Intervalo mínimo entre paquetes repetidos  |
| `zone_debounce_ms`     | int       | no        | `500`       | Tiempo de zona activa antes de desactivar  |
| `virtual_zones`        | lista     | no        | `[]`        | Lista de mapeos de zonas virtuales         |

**Sub-parámetros de zona virtual:**

| Parámetro        | Tipo    | Requerido | Por defecto | Descripción                             |
|------------------|---------|-----------|-------------|-----------------------------------------|
| `zone`           | int     | sí        | —           | Número de zona X-28 (1–32, según modelo) |
| `sensor_id`      | id      | sí        | —           | ID del sensor binario de ESPHome a monitorear |
| `trigger`        | string  | no        | `ON`        | Estado que dispara: `ON` u `OPEN`       |
| `zone_type`      | string  | no        | `MPXH`      | Tipo de paquete: `MPXH` o `WIRED`       |
| `clear_on_close` | boolean | no        | `true`      | Enviar paquete de restauración al cerrar |

## Uso en Home Assistant

Una vez que flasheaste el ESP y lo agregaste a Home Assistant (vía ESPHome API), las entidades aparecen automáticamente — no necesitas crear nada manualmente.

### Panel de Alarma en Lovelace

Agregá una tarjeta **Panel de Alarma** a tu dashboard:

1. Editá el dashboard → **Añadir tarjeta** → **Panel de alarma**
2. Seleccioná `alarm_control_panel.alarma_x28` (o el nombre que le hayas puesto)
3. Listo — podés armar/desarmar desde la UI

### Llamar Servicios desde Home Assistant

Los servicios de programación se usan desde **Developer Tools → Services**:

1. Andá a **Developer Tools → Services**
2. Buscá `x28_alarm.set_entry_delay` (o cualquier servicio de la lista)
3. Completá los parámetros y ejecutá

Ejemplo — cambiar demora de entrada a 30 segundos desde HA:

```yaml
service: x28_alarm.set_entry_delay
data:
  seconds: 30
```

### Automatizaciones

Dispará acciones cuando la alarma cambie de estado:

```yaml
automation:
  - alias: "Notificar alarma disparada"
    trigger:
      - platform: state
        entity_id: alarm_control_panel.alarma_x28
        to: "triggered"
    action:
      - service: notify.mobile_app_pablo
        data:
          message: "¡ALARMA DISPARADA!"
          title: "Alarma X-28"
```

### Botones en el Dashboard

Agregá los botones de pánico/fuego como tarjetas de botón en Lovelace:

1. Editá el dashboard → **Añadir tarjeta** → **Botón**
2. En **Entidad**, seleccioná `button.panico` (o `button.fuego`)
3. Opcional: cambiá el ícono y nombre

### Sniffer

Si habilitaste `sniffing.enabled: true`, aparecerá un `text_sensor` que muestra en tiempo real todos los paquetes del bus MPX. Agregalo como tarjeta de sensor en Lovelace para debuggear.

## Entidades

### Panel de Control (`alarm_control_panel`)

```yaml
alarm_control_panel:
  - platform: x28_alarm
    name: "Alarma X-28"
    id: panel_alarma
```

**Estados:**

| Estado HA      | Cuándo                            |
|----------------|-----------------------------------|
| DISARMED       | Paquete `0xC92B` recibido         |
| ARMED_AWAY     | Paquete `0x49C1` + último modo ME_VOY |
| ARMED_HOME     | Paquete `0x49C1` + último modo ESTOY |
| PENDING        | Después de comando armar/desarmar, esperando confirmación |
| TRIGGERED      | ARMADO + paquete de zona recibido |

### Sensores Binarios (`binary_sensor`)

```yaml
binary_sensor:
  - platform: x28_alarm
    name: "Modo Estoy"
    zone: ESTOY

  - platform: x28_alarm
    name: "Zona 1"
    zone: 1

  - platform: x28_alarm
    name: "Zona 2"
    zone: 2
```

### Botones (`button`)

```yaml
button:
  - platform: x28_alarm
    name: "Pánico"
    action: PANIC
  - platform: x28_alarm
    name: "Fuego"
    action: FIRE
  - platform: x28_alarm
    name: "Pánico Largo"
    action: PANIC_LONG
  - platform: x28_alarm
    name: "Fuego Largo"
    action: FIRE_LONG
```

### Sensor de Texto (`text_sensor` — Rastreador)

```yaml
text_sensor:
  - platform: x28_alarm
    name: "Rastreador Bus"
```

Solo disponible cuando `sniffing.enabled: true`.

## Servicios

### `x28_alarm.send_keys`

Envía una secuencia de teclas a la central. Permite programar cualquier
función de la alarma desde Home Assistant.

```yaml
service: x28_alarm.send_keys
data:
  keys: "467825PPpP88130F"
```

**Mapa de caracteres:**

| Carácter | Tecla          | Secuencia enviada             |
|----------|----------------|-------------------------------|
| `0`–`9`  | Dígitos        | Código del dígito             |
| `P`      | P (corto)      | `0x00AC`                      |
| `p`      | P (largo)      | `0x00AC` + `0x810A`           |
| `F`      | F (corto)      | `0x80BF`                      |
| `f`      | F (largo)      | `0x80BF` + `0x813C`           |
| `M`      | MODO           | `0x80DC`                      |
| `Z`      | ZONA (entrada) | `0x00CF`                       |
| `L`      | ZONA (salida)  | `0x00CF` + `0x8169`           |
| `!`      | Pánico         | `0x80EA`                      |
| `@`      | Fuego          | `0x00F9`                      |
| `#`      | Pánico largo   | `0x80EA` + `0x012F`           |
| `*`      | Fuego largo    | `0x00F9` + `0x0119`           |

### Servicios de Programación

El componente expone servicios de alto nivel para las operaciones de
programación más comunes, eliminando la necesidad de recordar códigos P.

#### Gestión de Modo Programación

| Servicio | Parámetros | Secuencia enviada | Descripción |
|----------|-----------|---------------|-------------|
| `enter_programming` | — | `<code>PP` | Entrar a programación básica |
| `enter_advanced_programming` | — | `<code>PPp` | Entrar a programación avanzada |
| `exit_programming` | — | `F` | Salir de programación |

#### Configuración del Panel (P-códigos)

Servicios autocontenidos: entran a programación avanzada, configuran y salen.

| Servicio | Parámetros | Secuencia enviada | Descripción |
|----------|-----------|---------------|-------------|
| `set_entry_delay` | `seconds: 5–99` | `<ic>PPpP881<SS>F` | Demora de entrada (segundos) |
| `set_exit_delay` | `seconds: 15–99` | `<ic>PPpP882<SS>F` | Demora de salida (segundos) |
| `set_siren_duration` | `minutes: 1–12` | `<ic>PPpP883<MM>F` | Duración de sirena A (minutos) |
| `set_siren_b_duration` | `minutes: 1–12` | `<ic>PPpP775<MM>F` | Duración de sirena B (minutos) |
| `set_clock_source` | `crystal: bool` | `<ic>PPpP777<N>F` | 0=red (50Hz), 1=cristal |
| `set_sabotage_inhibit` | `enabled: bool` | `<ic>PPpP774<N>F` | Inhibir sabotaje |
| `set_ac_frequency` | `hz: 50\|60` | `<ic>PPpP773<N>F` | Frecuencia de red |
| `set_entry_annunciator` | `enabled: bool` | `<ic>PPpP776<N>F` | Avisador acústico de entrada |
| `set_battery_save` | `enabled: bool` | `<ic>PPpP886<N>F` | Modo ahorro de batería |
| `set_owner_code_condition` | `disarm_only: bool` | `<ic>PPpP880<N>F` | 0=armar+desarmar, 1=solo desarmar |
| `set_zone_conditionality` | `enabled: bool` | `<ic>PPpP884<N>F` | 0=no, 1=sí (condicionalidad zonas 2 y 4) |
| `set_wired_zones` | `enabled: bool` | `<ic>PPpP885<N>F` | 0=deshabilitar, 1=habilitar zonas alámbricas |
| `set_partition_merge` | `enabled: bool` | `<ic>PPpP888<N>F` | 0=independientes, 1=hermanar particiones |
| `set_pgm_output` | `output, option, partition` | `<ic>PPpP77OOPF` | Configurar salida PGM |

`<ic>` = código de instalador configurado (por defecto `467825`).

#### Configuración de Zonas

| Servicio | Parámetros | Secuencia enviada | Descripción |
|----------|-----------|---------------|-------------|
| `save_estoy_config` | — | `<code>PPpP7781F` | Guarda modo Estoy como actual |
| `save_mevoy_config` | — | `<code>PPpP7782F` | Guarda modo Me Voy como actual |
| `set_zone_type` | `zone`, `type` | `<ic>PPpP99X<NN>F` | Cambia tipo de zona |
| `set_panic_zone` | — | `<ic>PPpP997F` | Designa zona como pánico |
| `set_tamper_zone` | — | `<ic>PPpP998F` | Designa zona como tamper |
| `toggle_zone_in_mode` | `zone: 1–N` | `Z<NN>` | Incluye/excluye zona en modo* |

`type` para `set_zone_type`: `normal` (robo normal, P994), `output_b` (solo sirena B, P991), `output_ab` (sirenas A+B, P992), `fire` (incendio, P993), `robbery` (robo normal, P994), `24h_protection` (protección 24h, P995), `fast_robbery` (robo rápida, P996). El rango de zonas depende del modelo.

#### Gestión de Códigos

| Servicio | Parámetros | Descripción |
|----------|-----------|-------------|
| `change_owner_code` | `new_code: 4–6 dígitos` | Cambia el PIN de usuario |
| `change_installer_code` | `new_code: 6 dígitos` | Cambia el código de instalador |
| `program_user` | `user`, `code`, `permissions`, `can_disarm` | Programa un código de usuario |

Parámetros de `program_user`:

| Parámetro | Valores | Descripción |
|-----------|--------|-------------|
| `user` | 1–30 | Número de usuario |
| `code` | 4–6 dígitos | PIN del usuario |
| `permissions` | 0=registrar, 1=Estoy, 2=Me Voy, 3=Cualquiera, 4=Asalto | Nivel de permiso |
| `can_disarm` | bool | Si puede desarmar |

#### Gestión de Sensores Inalámbricos (RF)

| Servicio | Parámetros | Descripción |
|----------|-----------|-------------|
| `rf_learn_mode` | — | Activa modo de aprendizaje RF |
| `rf_learn_slot` | `slot: 2–32` | Asigna slot en modo RF |
| `rf_delete_slot` | `slot: 2–32` | Elimina slot en modo RF |
| `exit_rf_learning` | — | Sale del modo RF |

#### Ejemplos

```yaml
# Cambiar demora de entrada a 30 segundos
service: x28_alarm.set_entry_delay
data: { seconds: 30 }

# Inhibir sabotaje
service: x28_alarm.set_sabotage_inhibit
data: { enabled: true }

# Cambiar código de usuario a 123456
service: x28_alarm.change_owner_code
data: { new_code: "123456" }

# Programar usuario 02 con código 4321, permiso 3, puede desarmar
service: x28_alarm.program_user
data:
  user: 2
  code: "4321"
  permissions: 3
  can_disarm: true

# Activar modo aprendizaje RF
service: x28_alarm.rf_learn_mode

# Cambiar duración de sirena B a 8 minutos
service: x28_alarm.set_siren_b_duration
data: { minutes: 8 }

# Cambiar fuente de reloj a cristal
service: x28_alarm.set_clock_source
data: { crystal: true }
```

## Zonas Virtuales

Las zonas virtuales permiten que sensores de Home Assistant (ej. sensores
Zigbee, Z-Wave, WiFi) se comporten como zonas nativas en la central X-28.
Cuando el sensor se activa, el ESP transmite el código de paquete de esa
zona al bus MPX, y la central reacciona como si fuera un sensor físico.

```yaml
x28_alarm:
  virtual_zones:
    - zone: 5
      sensor_id: binary_sensor.sensor_puerta
      trigger: ON
      zone_type: MPXH
      clear_on_close: true
```

## Compatibilidad de Modelos

Todas las centrales de la serie N (N4-MPXH, N8-MPXH, N8F-MPXH, N16-MPXH,
N32-MPXH, N32F-MPXH) y la serie 900X (9002-MPX, 9003-MPX, 9004-MPX) comparten
el mismo protocolo MPX/MPXH. El parámetro `model` ajusta la validación de
zonas y capacidades. En modo `AUTO`, el componente usa 32 zonas MPXH + 8
cableadas por defecto.

## Secuencias de Armado/Desarmado

| Acción        | Secuencia enviada      |
|---------------|------------------------|
| Armar Total   | MODO → `<PIN>`         |
| Armar Hogar   | MODO → `(esperar ESTOY)` → `<PIN>` |
| Desarmar      | `<PIN>`                |

El componente gestiona automáticamente el modo correcto (Estoy/Me Voy)
antes de enviar el PIN.

## Códigos de Zona Personalizados

Si los códigos de zona generados automáticamente no coinciden con tu central,
puedes sobrescribirlos:

```yaml
x28_alarm:
  zone_codes:
    5: 0x1655
    6: 0x1663
```

Usa el sniffer para capturar los códigos reales de tu panel. Las sobreescrituras
reemplazan la coincidencia tanto de MPXH como de cableado para la zona indicada.

## Solución de Problemas

**No se ven paquetes en el bus:**
- Verificar que `rx_pin` soporte interrupciones CHANGE
- Verificar polaridad: probar con `invert_rx: false`
- Comprobar el circuito de interfaz con un osciloscopio o lógica

**El ESP se reinicia al transmitir:**
- En ESP8266, deshabilitar interrupciones durante ~61 ms puede causar
  un watchdog WiFi. Considerar usar ESP32.

**La alarma no responde a los comandos:**
- Verificar que `code` coincida con el PIN configurado en la central
- Activar `debug: true` y observar los logs para ver los paquetes enviados
- Probar la secuencia manualmente con una tecla física

**El panel no confirma armado/desarmado:**
- El componente espera 10 segundos. Si no recibe el paquete de
  confirmación, revierte al estado anterior y registra una advertencia.

## Agradecimientos

Este componente no habría sido posible sin el trabajo de:

- **[fedapon/x28-mpx-controller](https://github.com/fedapon/x28-mpx-controller)** —
  Librería Arduino con implementación completa de TX/RX, buffer circular
  y eventos. Base del protocolo MPX.
- **[hjf/esphome-x28](https://github.com/hjf/esphome-x28)** — Primer componente
  ESPHome para X-28 usando la API `custom_component`. Inspiración para la
  integración con Home Assistant.
- **[gbisheimer/x28_sniffer](https://github.com/gbisheimer/x28_sniffer)** —
  Rastreador de protocolo basado en RCSwitch. Referencia para el decodificador
  y códigos de paquete conocidos.
- **X-28** — Por documentar públicamente los manuales de sus centrales,
  haciendo posible la ingeniería inversa del protocolo.
