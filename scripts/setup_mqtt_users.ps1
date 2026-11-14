# ============================================================
# Crea/rigenera un utente MQTT sul broker Mosquitto con password
# random. Se lo username e' "legmamiteo-backend", aggiorna anche
# il .env del backend; per ogni altro utente (serra-esp32, un
# osservatore per MQTT Explorer, ecc.) stampa solo le credenziali.
#
# Uso: .\setup_mqtt_user.ps1
# ============================================================

$ContainerName = "legmamiteo-mosquitto"
$PasswdFileInContainer = "/mosquitto/config/passwd"
$EnvFilePath = "C:\Users\gianb\Desktop\Projects\LegmaMiteo\BombaRepo\LegmaMiteo\.env"

function New-RandomPassword {
    $bytes = New-Object byte[] 24
    $rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
    $rng.GetBytes($bytes)
    $rng.Dispose()
    $b64 = [Convert]::ToBase64String($bytes) -replace '[+/=]', ''
    return $b64
}

$username = Read-Host "Nome utente MQTT da creare/rigenerare"
if ([string]::IsNullOrWhiteSpace($username)) {
    Write-Host "Nome utente vuoto, interrotto."
    exit 1
}

$password = New-RandomPassword

Write-Host ""
Write-Host "Creo/aggiorno l'utente '$username' sul broker..."
docker exec $ContainerName mosquitto_passwd -b $PasswdFileInContainer $username $password

Write-Host "Riavvio Mosquitto perche' rilegga gli utenti..."
docker restart $ContainerName | Out-Null

if ($username -eq "legmamiteo-backend") {
    if (Test-Path $EnvFilePath) {
        $envContent = Get-Content $EnvFilePath -Raw
    } else {
        $envContent = ""
    }

    $envContent = $envContent -replace '(?m)^MQTT_USER=.*$', ''
    $envContent = $envContent -replace '(?m)^MQTT_PASSWORD=.*$', ''
    $envContent = $envContent.TrimEnd() + "`nMQTT_USER=legmamiteo-backend`nMQTT_PASSWORD=$password`n"

    Set-Content -Path $EnvFilePath -Value $envContent -NoNewline

    Write-Host ""
    Write-Host "=== FATTO ==="
    Write-Host "Backend -> scritto in $EnvFilePath (MQTT_USER / MQTT_PASSWORD)"
}
else {
    Write-Host ""
    Write-Host "=== FATTO ==="
    Write-Host "Utente:   $username"
    Write-Host "Password: $password"
    Write-Host ""
    Write-Host "Se e' un dispositivo (firmware), copia i valori a mano nel codice"
    Write-Host "e riflasha. Se e' un client come MQTT Explorer, usali per la connessione."
}