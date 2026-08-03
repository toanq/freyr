FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
# vieneu's CPU path (ONNX Runtime) is torch-free -- unlike the previous
# facebook/mms-tts-vie backend, no separate CPU-only-wheel torch install
# step is needed here.
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py .

EXPOSE 8000
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
