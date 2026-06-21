from(bucket:"stations")
  |> range(start: -30d)
  |> filter(fn: (r) => r._measurement == "weather_station")
  |> limit(n:5)