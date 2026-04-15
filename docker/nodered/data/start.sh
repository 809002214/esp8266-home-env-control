#!/bin/sh
set -eu

cd /data

echo "Starting Node-RED bootstrap..."

if [ ! -d /data/node_modules/@flowfuse/node-red-dashboard ]; then
  echo "Installing FlowFuse Dashboard nodes..."
  npm install --unsafe-perm --no-update-notifier --no-fund --omit=dev
fi

if [ ! -f /data/flows.json ]; then
  echo "Generating initial flows.json from template..."
  sed \
    -e "s|__MQTT_BROKER__|${MQTT_BROKER:-mqtt}|g" \
    -e "s|__MQTT_PORT__|${MQTT_PORT:-1883}|g" \
    -e "s|__MQTT_USERNAME__|${MQTT_USERNAME:-mqtt}|g" \
    -e "s|__MQTT_PASSWORD__|${MQTT_PASSWORD:-123456}|g" \
    /data/flows.template.json > /data/flows.json
fi

exec node /usr/src/node-red/node_modules/node-red/red.js --userDir /data "${FLOWS:-flows.json}"
