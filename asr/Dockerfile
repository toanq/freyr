FROM python:3.11-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# CPU-only wheel index -- the default PyPI torch package bundles CUDA and is
# ~10x larger than needed for this CPU-only sidecar.
RUN pip install --no-cache-dir torch==2.5.1 --index-url https://download.pytorch.org/whl/cpu

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# ctranslate2's prebuilt .so ships without the correct PT_GNU_STACK marking,
# so the kernel here refuses to map it ("cannot enable executable stack as
# shared object requires: Invalid argument"). Neither `execstack` (dropped
# from Debian trixie's repos) nor this binutils version's `elfedit` (no
# --execstack-clear option) are available/work here -- clear_execstack.py
# patches the ELF program header directly instead.
COPY clear_execstack.py .
RUN find /usr/local/lib/python3.11/site-packages -iname "*ctranslate2*.so*" -print0 \
    | xargs -0 python3 clear_execstack.py

COPY app.py entrypoint.sh ./
RUN chmod +x entrypoint.sh

ENV PHOWHISPER_MODEL_DIR=/models/phowhisper-small-ct2
VOLUME /models

EXPOSE 8000
ENTRYPOINT ["./entrypoint.sh"]
