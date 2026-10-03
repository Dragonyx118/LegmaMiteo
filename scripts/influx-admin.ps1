# ===========================================
# LegmaMiteo - Admin InfluxDB
# ===========================================
# Strumento interattivo per controllare e cancellare dati nel database,
# senza dover scrivere a mano query Flux o comandi "influx delete" con
# conversioni di fuso orario manuali (fonte di errori nei mesi scorsi).
#
# NOTA TECNICA: i comandi Flux/influx non vengono passati come argomenti
# a "docker exec" (PowerShell tende a "mangiare" le virgolette quando le
# passa a un comando esterno come docker.exe, causando errori tipo
# "undefined identifier"). Vengono invece scritti in un file temporaneo
# locale, copiato dentro al container con "docker cp", ed eseguito da
# li'. Questo e' lo stesso motivo per cui in passato i comandi
# "influx delete" funzionavano solo entrando a mano dentro una shell del
# container (docker exec -it ... sh) invece che lanciandoli direttamente.
#
# Uso: lancialo da PowerShell nella cartella "server" (dove sta
# docker-compose.yml), oppure da qualsiasi cartella: aggiusta $envPath
# sotto se il file .env non e' a fianco allo script.

$containerName = "legmamiteo-influxdb"
$envPath = ".\.env"
$bucketDefault = "stations"
$orgDefault = "legmamiteo"
$measurementDefault = "weather_station"
$hostDefault = "legmamiteo-server"

$fusoOrario = [System.TimeZoneInfo]::FindSystemTimeZoneById("W. Europe Standard Time")

function Locale-a-Utc([datetime]$dataLocale) {
    $dataSpecificata = [System.DateTime]::SpecifyKind($dataLocale, [System.DateTimeKind]::Unspecified)
    $utc = [System.TimeZoneInfo]::ConvertTimeToUtc($dataSpecificata, $fusoOrario)
    return $utc.ToString("yyyy-MM-ddTHH:mm:ssZ")
}

function Leggi-EnvVar([string]$nome, [string]$default) {
    if (Test-Path $envPath) {
        $riga = Get-Content $envPath | Where-Object { $_ -match "^$nome=" }
        if ($riga) {
            return ($riga -split "=", 2)[1].Trim()
        }
    }
    return $default
}

# Esegue uno script shell dentro al container copiandolo prima come
# file, invece di passarlo come argomento (evita ogni problema di
# escaping/virgolette tra PowerShell, docker.exe e la shell interna).
function Esegui-NelContainer([string]$scriptShell) {
    $tempLocale = [System.IO.Path]::GetTempFileName()
    $tempLocale = [System.IO.Path]::ChangeExtension($tempLocale, ".sh")
    $contenutoUnix = $scriptShell -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($tempLocale, $contenutoUnix)

    docker cp $tempLocale "${containerName}:/tmp/influx-admin-cmd.sh" | Out-Null
    $risultato = docker exec $containerName sh /tmp/influx-admin-cmd.sh 2>&1
    docker exec $containerName rm -f /tmp/influx-admin-cmd.sh | Out-Null
    Remove-Item $tempLocale -ErrorAction SilentlyContinue

    return $risultato
}

$token = Leggi-EnvVar "INFLUX_TOKEN" $null
$org = Leggi-EnvVar "INFLUX_ORG" $orgDefault
$bucket = Leggi-EnvVar "INFLUX_BUCKET" $bucketDefault

if (-not $token) {
    $token = Read-Host "Token InfluxDB non trovato in .env - incollalo qui"
}

Write-Host ""
Write-Host "=== LegmaMiteo - Admin InfluxDB ===" -ForegroundColor Cyan
Write-Host "Bucket: $bucket | Org: $org" -ForegroundColor DarkGray
Write-Host ""
Write-Host "1) Visualizza dati in un periodo (senza cancellare nulla)"
Write-Host "2) Cancella dati in un periodo"
Write-Host "0) Esci"
$scelta = Read-Host "`nScegli"

if ($scelta -eq "0") { exit 0 }

$measurement = Read-Host "Measurement [invio per '$measurementDefault']"
if (-not $measurement) { $measurement = $measurementDefault }

$hostFiltro = Read-Host "Host [invio per '$hostDefault']"
if (-not $hostFiltro) { $hostFiltro = $hostDefault }

Write-Host "`nInserisci il periodo in ORA LOCALE ITALIANA (gestisce da sola legale/solare)." -ForegroundColor Yellow
$inizioStr = Read-Host "Inizio (es. 2026-09-27 15:20:00)"
$fineStr = Read-Host "Fine   (es. 2026-09-27 15:21:00)"

try {
    $inizioLocale = [datetime]::ParseExact($inizioStr, "yyyy-MM-dd HH:mm:ss", $null)
    $fineLocale = [datetime]::ParseExact($fineStr, "yyyy-MM-dd HH:mm:ss", $null)
} catch {
    Write-Host "Formato data non valido. Usa: yyyy-MM-dd HH:mm:ss" -ForegroundColor Red
    exit 1
}

$inizioUtc = Locale-a-Utc $inizioLocale
$fineUtc = Locale-a-Utc $fineLocale

Write-Host "`nRange UTC calcolato: $inizioUtc -> $fineUtc" -ForegroundColor DarkGray

# --- Anteprima: conta quanti punti ci sono nel range prima di decidere
# se cancellare. La query Flux viene scritta in un file dentro al
# container tramite "cat > ... << 'EOF' ... EOF" (heredoc con delimitatore
# tra apici singoli: nessuna sostituzione di variabili shell, il
# contenuto passa esattamente com'e'). ---
$fluxQuery = @"
from(bucket: "$bucket")
  |> range(start: $inizioUtc, stop: $fineUtc)
  |> filter(fn: (r) => r._measurement == "$measurement" and r.host == "$hostFiltro")
  |> count()
"@

$scriptConteggio = @"
cat > /tmp/preview.flux << 'FLUXEOF'
$fluxQuery
FLUXEOF
influx query -f /tmp/preview.flux --org '$org' --token '$token'
rm -f /tmp/preview.flux
"@

Write-Host "`nConto i punti nel range..." -ForegroundColor DarkGray
$risultatoConteggio = Esegui-NelContainer $scriptConteggio
Write-Host $risultatoConteggio

if ($scelta -eq "1") {
    Write-Host "`nFatto - nessuna modifica effettuata (solo visualizzazione)." -ForegroundColor Green
    exit 0
}

# --- Cancellazione: conferma esplicita, irreversibile ---
Write-Host "`nATTENZIONE: la cancellazione e' IRREVERSIBILE." -ForegroundColor Red
Write-Host "Measurement: $measurement | Host: $hostFiltro"
Write-Host "Periodo locale: $inizioStr -> $fineStr"
Write-Host "Periodo UTC:    $inizioUtc -> $fineUtc"
$conferma = Read-Host "`nScrivi ESATTAMENTE 'cancella' per procedere"

if ($conferma -ne "cancella") {
    Write-Host "Annullato, nessuna modifica effettuata." -ForegroundColor Yellow
    exit 0
}

$predicateShell = "_measurement=`"$measurement`" AND host=`"$hostFiltro`""

$scriptDelete = @"
influx delete --bucket '$bucket' --org '$org' --start '$inizioUtc' --stop '$fineUtc' --predicate '$predicateShell' --token '$token'
"@

$risultatoDelete = Esegui-NelContainer $scriptDelete
Write-Host $risultatoDelete

Write-Host "`nCancellazione completata." -ForegroundColor Green