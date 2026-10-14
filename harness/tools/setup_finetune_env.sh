#!/bin/sh
# One-time GPU venv for the title fine-tune (host box).
# Run from anywhere:  sh tools/setup_finetune_env.sh
set -e
HARNESS=$(cd "$(dirname "$0")/.." && pwd)
VENV="$HARNESS/models/safetensors/title/.venv"

python -m venv "$VENV"
if [ -x "$VENV/bin/python" ]; then
    PY="$VENV/bin/python"          # Unix layout
else
    PY="$VENV/Scripts/python.exe"  # Windows layout
fi
"$PY" -m pip install --upgrade pip
"$PY" -m pip install \
  "torch>=2.4" \
  "transformers>=4.51" \
  "peft>=0.12" \
  "trl>=0.15" \
  "datasets>=2.20" \
  "accelerate>=0.34" \
  "safetensors>=0.4"

"$PY" - <<'EOF'
import torch
print("torch:", torch.__version__)
print("cuda:", torch.cuda.is_available())
if torch.cuda.is_available():
    print("device:", torch.cuda.get_device_name(0))
EOF
echo "venv ready: $PY"
