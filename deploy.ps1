# Copie l'appli sur l'UNO Q et la demarre (arrete l'appli en cours). Windows / PowerShell.
# Usage : powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Hote 192.168.1.42]   (defaut : uno-q.local)
# Prerequis : acces SSH par cle a l'utilisateur arduino de la carte.
param([string]$Hote = "uno-q.local")
$H = "arduino@$Hote"
$App = "minitel-unoq"
Set-Location $PSScriptRoot
ssh $H "mkdir -p ~/ArduinoApps/$App/sketch ~/ArduinoApps/$App/python"
scp -q app.yaml "${H}:ArduinoApps/$App/"
scp -q sketch/sketch.ino sketch/sketch.yaml "${H}:ArduinoApps/$App/sketch/"
scp -q python/main.py "${H}:ArduinoApps/$App/python/"
ssh $H 'for a in $(arduino-app-cli app list | awk ''$NF!="true" && /running/ {print $1}''); do arduino-app-cli app stop $a; done; arduino-app-cli app start user:minitel-unoq'
