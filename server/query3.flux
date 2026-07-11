from(bucket:"stations")
  |> range(start: -24h)
  |> sort(columns: ["_time"], desc: true)
  |> limit(n: 5)