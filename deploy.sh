#!/usr/bin/env bash
# Copie l'appli sur l'UNO Q et la demarre (arrete l'appli en cours).
# Usage : ./deploy.sh [hote]      ex. ./deploy.sh 192.168.1.42   (defaut : uno-q.local)
# Prerequis : acces SSH par cle a l'utilisateur arduino de la carte.
set -e
HOST="arduino@${1:-uno-q.local}"
APP=minitel-unoq
cd "$(dirname "$0")"
ssh "$HOST" "mkdir -p ~/ArduinoApps/$APP/sketch ~/ArduinoApps/$APP/python"
scp -q app.yaml "$HOST:ArduinoApps/$APP/"
scp -q sketch/sketch.ino sketch/sketch.yaml "$HOST:ArduinoApps/$APP/sketch/"
scp -q python/main.py "$HOST:ArduinoApps/$APP/python/"
ssh "$HOST" "for a in \$(arduino-app-cli app list | awk '\$NF!=\"true\" && /running/ {print \$1}'); do arduino-app-cli app stop \$a; done; arduino-app-cli app start user:$APP"
