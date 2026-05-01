# Source this from zsh after the pixi environment is active:
#   source scripts/primeparts-completion.zsh

if command -v register-python-argcomplete >/dev/null 2>&1; then
  eval "$(register-python-argcomplete --shell zsh primeparts)"
fi
