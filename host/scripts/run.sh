#!/bin/sh
# Se invoca desde host/ mediante make run. Las cargas son secuenciales,
# incluso con make -j; el host solo arranca cuando ambas terminan bien.
set -eu

config=$1
pio=$2

die() {
    printf 'make run: %s\n' "$*" >&2
    exit 1
}

[ -r "$config" ] || die "no se puede leer la configuración: $config"

# Leer las mismas claves que el host, conservando espacios dentro de las rutas.
read_port() {
    awk -v wanted="$1" '
        function trim(s) { sub(/^[ \t\r]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }
        {
            line = trim($0)
            if (line == "" || substr(line, 1, 1) == "#") next
            eq = index(line, "=")
            if (eq && trim(substr(line, 1, eq - 1)) == wanted) {
                count++
                port = trim(substr(line, eq + 1))
            }
        }
        END { if (count != 1 || port == "") exit 1; print port }
    ' "$config"
}

# BOOTSEL usa USB directo (2e8a:0003), sin puerto ttyACM. Leer sysfs permite
# detectarlo aunque todavía falten los permisos de libusb/picotool.
pico_in_bootsel() {
    for device in /sys/bus/usb/devices/*; do
        [ -r "$device/idVendor" ] && [ -r "$device/idProduct" ] || continue
        [ "$(cat "$device/idVendor")" = 2e8a ] &&
        [ "$(cat "$device/idProduct")" = 0003 ] && return 0
    done
    return 1
}

check_port() {
    [ -c "$1" ] || die "no existe el puerto $1; conecta la placa y revisa $config"
    [ -r "$1" ] && [ -w "$1" ] || die "sin acceso de lectura/escritura a $1"
}

esp_port=$(read_port esp_port) || die "esp_port debe aparecer una vez y tener valor en $config"
pico_port=$(read_port pico_port) || die "pico_port debe aparecer una vez y tener valor en $config"
[ "$esp_port" != "$pico_port" ] || die "ESP32 y Pico requieren puertos distintos"
check_port "$esp_port"
if pico_in_bootsel; then
    printf 'Pico detectada en BOOTSEL: se puede cargar aunque no exista %s.\n' "$pico_port"
elif [ -e "$pico_port" ]; then
    check_port "$pico_port"
    [ ! "$esp_port" -ef "$pico_port" ] || die "los dos puertos apuntan al mismo dispositivo"
else
    die "no existe $pico_port ni se detectó una Pico en BOOTSEL. Revisa $config; para recuperar la placa, conecta USB manteniendo BOOTSEL pulsado. Si hubo errores de permisos, ejecuta make setup-usb"
fi
command -v "$pio" >/dev/null 2>&1 || die "PlatformIO no está disponible; indica PIO=/ruta/al/pio"
"$pio" --version

printf '\nCompilando firmware ESP32 y Pico...\n'
"$pio" run --project-dir ../nodo1ESP/ESP_RT_Timer --environment esp32dev
"$pio" run --project-dir ../node2 --environment pico

printf '\nCargando ESP32 en %s...\n' "$esp_port"
"$pio" run --project-dir ../nodo1ESP/ESP_RT_Timer --environment esp32dev --target upload --upload-port "$esp_port"
printf '\nCargando Pico en %s...\n' "$pico_port"
if ! "$pio" run --project-dir ../node2 --environment pico --target upload --upload-port "$pico_port"; then
    die "falló la carga de Pico. Si picotool indica permisos insuficientes, ejecuta make setup-usb y vuelve a intentar make run; en BOOTSEL es normal que no exista $pico_port"
fi

# La Pico puede desconectarse y volver a enumerarse después de la carga.
# Esta espera pertenece al despliegue; la temporización de tareas usa el timer.
attempt=0
stable=0
while [ "$attempt" -lt 50 ]; do
    sleep 0.2
    attempt=$((attempt + 1))
    if [ -c "$esp_port" ] && [ -r "$esp_port" ] && [ -w "$esp_port" ] &&
       [ -c "$pico_port" ] && [ -r "$pico_port" ] && [ -w "$pico_port" ]; then
        stable=$((stable + 1))
        [ "$stable" -lt 3 ] || break
    else
        stable=0
    fi
done
[ "$stable" -ge 3 ] || die "los puertos no están disponibles tras la carga; revisa $config"

printf '\nIniciando host con %s (q para detener nodos y generar el reporte)...\n' "$config"
exec ./build/host "$config"
