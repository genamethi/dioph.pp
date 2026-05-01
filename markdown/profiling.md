 Usage patterns:
  # Composable — any mode, any command
  pixi run -e dev profile pyspy funbuns --remainder
  pixi run -e dev profile trace funbuns --spectral

  # Shortcuts
  pixi run -e dev profile-ladic
  pixi run -e dev profile-remainder

  # Or just py-spy directly (default env, no -e dev needed)
  pixi run py-spy record --native -o logs/profile.svg -- funbuns --ladic

  # Attach to already-running process
  pixi run py-spy top --pid $(pgrep -f "funbuns --ladic")
  pixi run py-spy record -o logs/profile.svg --pid $(pgrep -f "funbuns --ladic")

  The --native flag is key — it shows Rust/C frames from the GMP factorization code alongside Python frames, so you'll see exactly how much time is GMP vs Polars vs Python
  overhead.
