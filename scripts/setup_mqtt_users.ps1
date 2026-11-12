# ============================================================
# Crea/rigenera gli utenti MQTT "legmamiteo-backend" e "serra-esp32"
# sul broker Mosquitto con password random, e aggiorna il .env del
# backend con le credenziali del backend.
#
# Uso: modifica $EnvFilePath qui sotto col path reale del tuo
# .env, poi lancia: .\setup_mqtt_users.ps1
# ============================================================

$ContainerName = "legmamiteo-mosquitto"
$PasswdFileInContainer = "/mosquitto/config/passwd"
$EnvFilePath = "C:\Users\gianb\Desktop\Projects\LegmaMiteo\BombaRepo\LegmaMiteo\.env"

function New-RandomPassword {
    # RNGCryptoServiceProvider invece di RandomNumberGenerator::Fill, che
    # esiste solo in .NET 6+ e non in Windows PowerShell 5.1 (.NET Framework)
    $bytes = New-Object byte[] 24
    $rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
    $rng.GetBytes($bytes)
    $rng.Dispose()
    $b64 = [Convert]::ToBase64String($bytes) -replace '[+/=]', ''
    return $b64
}

$backendPassword = New-RandomPassword
$serraPassword = New-RandomPassword

Write-Host "Verifica: le due password devono essere diverse tra loro"
Write-Host "Backend: $backendPassword"
Write-Host "Serra:   $serraPassword"
Write-Host ""

Write-Host "Creo/aggiorno l'utente 'legmamiteo-backend' sul broker..."
docker exec $ContainerName mosquitto_passwd -b $PasswdFileInContainer "legmamiteo-backend" $backendPassword

Write-Host "Creo/aggiorno l'utente 'serra-esp32' sul broker..."
docker exec $ContainerName mosquitto_passwd -b $PasswdFileInContainer "serra-esp32" $serraPassword

Write-Host "Riavvio Mosquitto perche' rilegga gli utenti..."
docker restart $ContainerName | Out-Null

# --- Aggiorna/aggiunge le righe MQTT_USER / MQTT_PASSWORD nel .env del backend ---
if (Test-Path $EnvFilePath) {
    $envContent = Get-Content $EnvFilePath -Raw
} else {
    $envContent = ""
}

$envContent = $envContent -replace '(?m)^MQTT_USER=.*$', ''
$envContent = $envContent -replace '(?m)^MQTT_PASSWORD=.*$', ''
$envContent = $envContent.TrimEnd() + "`nMQTT_USER=legmamiteo-backend`nMQTT_PASSWORD=$backendPassword`n"

Set-Content -Path $EnvFilePath -Value $envContent -NoNewline

Write-Host ""
Write-Host "=== FATTO ==="
Write-Host "Backend -> scritto in $EnvFilePath (MQTT_USER / MQTT_PASSWORD)"
Write-Host ""
Write-Host "Serra -> copia questi valori a mano in SegmaSirra_main.cpp:"
Write-Host "  SERRA_MQTT_USER     = `"serra-esp32`""
Write-Host "  SERRA_MQTT_PASSWORD = `"$serraPassword`""
Write-Host ""
Write-Host "(la password della serra non puo' essere scritta automaticamente"
Write-Host " nel firmware: va ricompilato e riflashato sull'ESP32 a mano)"