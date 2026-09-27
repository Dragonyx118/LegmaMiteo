# ===========================================
# LegmaMiteo — Self-heal automatico Funnel/DNS Tailscale
# ===========================================
# Verifica periodicamente se il DNS pubblico del Funnel risolve
# correttamente. Se fallisce, prova prima un semplice restart del
# container; se anche questo non basta, forza un logout+re-registrazione
# completa (lo stesso intervento manuale che risolveva il problema).
#
# Pensato per girare come Attività Pianificata di Windows ogni 10-15
# minuti, così il problema si ripara da solo senza intervento manuale.

$hostname = "legmamiteo-server.tail1c95b4.ts.net"
$containerName = "legmamiteo-tailscale"   # Nome del container: usato con "docker exec"
$serviceName = "tailscale"                 # Nome del servizio nel docker-compose.yml: usato con "docker compose"
$logFile = "$PSScriptRoot\selfheal.log"
$statoFile = "$PSScriptRoot\selfheal-stato.json"

function Scrivi-Log($messaggio) {
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    "$timestamp - $messaggio" | Out-File -FilePath $logFile -Append -Encoding utf8
}

function Test-DnsPubblico {
    try {
        $risultato = Resolve-DnsName -Name $hostname -Server 8.8.8.8 -Type A -ErrorAction Stop
        return $risultato.Count -gt 0
    } catch {
        return $false
    }
}

function Test-ServerAttivo {
    # Verifica che il resto dello stack (non solo Tailscale) sia
    # effettivamente in esecuzione, cioè che Docker Desktop sia aperto e
    # i container principali siano su. Se il server è spento
    # intenzionalmente, non ha senso tentare nessuna riparazione: non
    # aggiunge nulla e potrebbe fare un logout inutile mentre semplicemente
    # non hai ancora acceso nulla.
    try {
        $statoMosquitto = docker inspect --format='{{.State.Running}}' legmamiteo-mosquitto 2>$null
        $statoInfluxdb  = docker inspect --format='{{.State.Running}}' legmamiteo-influxdb 2>$null
        return ($statoMosquitto -eq "true") -and ($statoInfluxdb -eq "true")
    } catch {
        return $false
    }
}

if (-not (Test-ServerAttivo)) {
    Scrivi-Log "Server non attivo (Docker Desktop chiuso o container fermi): nessuna azione, esco."
    exit 0
}

# Legge quanti tentativi di riparazione consecutivi sono già stati fatti,
# per evitare di fare logout ripetuti a raffica se il problema persiste
# per motivi esterni (es. vero disservizio Tailscale).
$tentativiConsecutivi = 0
if (Test-Path $statoFile) {
    $stato = Get-Content $statoFile | ConvertFrom-Json
    $tentativiConsecutivi = $stato.tentativiConsecutivi
}

if (Test-DnsPubblico) {
    Scrivi-Log "OK: DNS pubblico risolve correttamente. Nessuna azione necessaria."
    @{ tentativiConsecutivi = 0 } | ConvertTo-Json | Out-File -FilePath $statoFile -Encoding utf8
    exit 0
}

Scrivi-Log "PROBLEMA: DNS pubblico NON risolve per $hostname."

if ($tentativiConsecutivi -eq 0) {
    # Primo fallimento: prova prima il tentativo più leggero (restart semplice).
    Scrivi-Log "Tentativo 1: restart semplice del container Tailscale."
    docker compose restart $serviceName 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
    Start-Sleep -Seconds 15
    docker exec $containerName tailscale funnel --bg 3000 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
    docker exec $containerName tailscale funnel --bg --tcp 8443 8883 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
} elseif ($tentativiConsecutivi -ge 2) {
    # Fallito già 2 volte di fila: passa al logout+re-registrazione completa.
    Scrivi-Log "Tentativo $($tentativiConsecutivi + 1): restart semplice non bastava, provo logout + re-registrazione completa."
    docker exec $containerName tailscale logout 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
    docker compose restart $serviceName 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
    Start-Sleep -Seconds 20
    docker exec $containerName tailscale funnel --bg 3000 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8
    docker exec $containerName tailscale funnel --bg --tcp 8443 8883 2>&1 | Out-File -FilePath $logFile -Append -Encoding utf8

    # ATTENZIONE: dopo un logout, l'hostname potrebbe cambiare (es. tornare
    # con suffisso -1 se il vecchio nodo non si è ancora liberato). Il
    # log seguente aiuta a scoprirlo subito, ma servirà comunque
    # aggiornare manualmente secrets.h se è cambiato.
    $statoAttuale = docker exec $containerName tailscale status 2>&1
    Scrivi-Log "Stato Tailscale dopo re-registrazione: $statoAttuale"
} else {
    Scrivi-Log "Tentativo $($tentativiConsecutivi + 1): attendo (il restart precedente potrebbe ancora dover propagare)."
}

@{ tentativiConsecutivi = $tentativiConsecutivi + 1 } | ConvertTo-Json | Out-File -FilePath $statoFile -Encoding utf8