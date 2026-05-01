# Source this from bash after the pixi environment is active:
#   source scripts/primeparts-completion.bash

if command -v register-python-argcomplete >/dev/null 2>&1; then
  eval "$(register-python-argcomplete --shell bash primeparts)"
fi
