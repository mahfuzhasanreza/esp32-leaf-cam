import io
import json
import os
import threading
from datetime import datetime
from typing import Dict, Optional, Tuple

import numpy as np
import torch
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from PIL import Image

app = FastAPI()

BASE_DIR = os.path.dirname(__file__)

# Directory where incoming images are stored so the ESP32 can fetch results later.
UPLOAD_DIR = os.path.join(BASE_DIR, "upload")
os.makedirs(UPLOAD_DIR, exist_ok=True)

_TFLITE_MODEL_PATH = os.path.join(BASE_DIR, "leaf_resnet50_float.tflite")
_LABEL_TXT_PATH = os.path.join(BASE_DIR, "leaf_labels.txt")
_LABEL_JSON_PATH = os.path.join(BASE_DIR, "leaf_labels.json")

_tflite_interpreter = None
_tflite_input_details = None
_tflite_output_details = None
_tflite_lock = threading.Lock()
_tflite_labels: Optional[list[str]] = None
_label_metadata: Optional[Dict[str, Dict[str, str]]] = None

# Cache the latest analysis so the ESP32 hub can poll /result and refresh its OLED.
_latest_result: Optional[Dict[str, object]] = None


def _kaggle_env_labels() -> Optional[list[str]]:
    raw_path = os.path.join(BASE_DIR, "leaf_labels.json")
    if os.path.exists(raw_path):
        try:
            with open(raw_path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, json.JSONDecodeError):
            return None
        if isinstance(data, dict) and "labels" in data and isinstance(data["labels"], list):
            return [str(item) for item in data["labels"]]
    return None


def _load_label_metadata() -> None:
    global _tflite_labels, _label_metadata
    labels: list[str] = []

    if os.path.exists(_LABEL_TXT_PATH):
        try:
            with open(_LABEL_TXT_PATH, "r", encoding="utf-8") as fh:
                labels = [line.strip() for line in fh if line.strip()]
        except OSError as exc:
            print(f"[pi5_server] Failed to load label file '{_LABEL_TXT_PATH}': {exc}")

    if not labels:
        kaggle_labels = _kaggle_env_labels()
        if kaggle_labels:
            labels = kaggle_labels

    _tflite_labels = labels or None

    if os.path.exists(_LABEL_JSON_PATH):
        try:
            with open(_LABEL_JSON_PATH, "r", encoding="utf-8") as fh:
                data = json.load(fh)
                if isinstance(data, dict):
                    _label_metadata = data
        except (OSError, json.JSONDecodeError) as exc:
            print(f"[pi5_server] Failed to load label metadata '{_LABEL_JSON_PATH}': {exc}")

def _load_tflite_interpreter() -> None:
    """Load the TFLite model if available."""
    global _tflite_interpreter, _tflite_input_details, _tflite_output_details
    if not os.path.exists(_TFLITE_MODEL_PATH):
        return
    try:
        try:
            from tflite_runtime.interpreter import Interpreter
        except ImportError:
            from tensorflow.lite.python.interpreter import Interpreter  # type: ignore
    except ImportError:
        print("[pi5_server] No TFLite runtime available; using heuristic analysis.")
        return

    try:
        interpreter = Interpreter(model_path=_TFLITE_MODEL_PATH)
        interpreter.allocate_tensors()
        _tflite_interpreter = interpreter
        _tflite_input_details = interpreter.get_input_details()
        _tflite_output_details = interpreter.get_output_details()
        print(f"[pi5_server] Loaded TFLite model from '{_TFLITE_MODEL_PATH}'.")
    except Exception as exc:  # noqa: BLE001
        print(f"[pi5_server] Failed to load TFLite model: {exc}")
        _tflite_interpreter = None
        _tflite_input_details = None
        _tflite_output_details = None


def _friendly_label(raw_label: str, index: int) -> str:
    if raw_label:
        return raw_label.replace("_", " ").title()
    return f"Class {index}"


def _heuristic_analysis(pil_img: Image.Image) -> Dict[str, object]:
    """Fallback heuristic analysis when the model is unavailable."""
    data = torch.tensor(list(pil_img.getdata()), dtype=torch.float32) / 255.0
    if data.numel() == 0:
        return {
            "leaf_name": "Unknown Leaf",
            "disease": "No image data",
            "solution": "Re-capture the image.",
            "metrics": {},
        }

    data = data.view(pil_img.height, pil_img.width, 3)
    red = data[..., 0].mean().item()
    green = data[..., 1].mean().item()
    blue = data[..., 2].mean().item()
    brightness = float(data.mean().item())
    chlorophyll_score = green - 0.5 * (red + blue)
    dryness_score = red - green

    if brightness < 0.2:
        leaf = "Underexposed Leaf"
    elif chlorophyll_score > 0.05:
        leaf = "Healthy Leaf"
    else:
        leaf = "Stressed Leaf"

    if dryness_score > 0.08:
        disease = "Possible Leaf Scorch"
        solution = "Increase watering and check for pests."
    elif green < 0.3:
        disease = "Nutrient Deficiency Suspected"
        solution = "Apply balanced fertilizer and monitor."
    else:
        disease = "No obvious disease"
        solution = "Continue regular care."

    return {
        "leaf_name": leaf,
        "disease": disease,
        "solution": solution,
        "metrics": {
            "brightness": round(brightness, 3),
            "mean_red": round(red, 3),
            "mean_green": round(green, 3),
            "mean_blue": round(blue, 3),
            "chlorophyll_score": round(chlorophyll_score, 3),
            "dryness_score": round(dryness_score, 3),
            "analysis_source": "heuristic",
        },
    }


def _analyze_with_model(pil_img: Image.Image, heuristics: Dict[str, object]) -> Optional[Dict[str, object]]:
    if _tflite_interpreter is None or _tflite_input_details is None or _tflite_output_details is None:
        return None

    try:
        with _tflite_lock:
            input_detail = _tflite_input_details[0]
            input_shape = input_detail["shape"]
            height, width = input_shape[1], input_shape[2]
            model_dtype = input_detail["dtype"]

            resized = pil_img.resize((width, height))
            np_img = np.asarray(resized)

            if model_dtype == np.float32:
                np_img = np_img.astype(np.float32) / 255.0
            else:
                np_img = np_img.astype(model_dtype)

            np_img = np.expand_dims(np_img, axis=0)
            _tflite_interpreter.set_tensor(input_detail["index"], np_img)
            _tflite_interpreter.invoke()
            output_data = _tflite_interpreter.get_tensor(_tflite_output_details[0]["index"])
    except Exception as exc:  # noqa: BLE001
        print(f"[pi5_server] TFLite inference failed: {exc}")
        return None

    probabilities = np.squeeze(output_data)
    if probabilities.ndim == 0:
        probabilities = np.array([probabilities])

    top_index = int(np.argmax(probabilities))
    confidence = float(probabilities[top_index])
    raw_label = ""
    if _tflite_labels and top_index < len(_tflite_labels):
        raw_label = _tflite_labels[top_index]
    friendly = _friendly_label(raw_label, top_index)

    result = {
        "leaf_name": friendly,
        "disease": heuristics["disease"],
        "solution": heuristics["solution"],
        "metrics": {
            **heuristics.get("metrics", {}),
            "model_confidence": round(confidence, 4),
            "model_class_index": top_index,
            "model_label": raw_label or friendly,
            "analysis_source": "tflite_model",
        },
    }

    if _label_metadata and raw_label in _label_metadata:
        meta = _label_metadata[raw_label]
        result["disease"] = meta.get("disease", result["disease"])
        result["solution"] = meta.get("solution", result["solution"])
    elif _label_metadata and friendly in _label_metadata:
        meta = _label_metadata[friendly]
        result["disease"] = meta.get("disease", result["disease"])
        result["solution"] = meta.get("solution", result["solution"])

    return result


def _analyze_image(img_bytes: bytes) -> Dict[str, object]:
    with Image.open(io.BytesIO(img_bytes)) as pil_img:
        pil_img = pil_img.convert("RGB")
        heuristics = _heuristic_analysis(pil_img)
        model_result = _analyze_with_model(pil_img, heuristics)
        return model_result or heuristics


def _kaggle_credentials() -> Tuple[Optional[str], Optional[str]]:
    kaggle_json_path = os.path.join(BASE_DIR, "kaggle.json")
    alt_path = os.path.join(BASE_DIR, "kaggle (1).json")

    if not os.path.exists(kaggle_json_path) and os.path.exists(alt_path):
        try:
            os.rename(alt_path, kaggle_json_path)
            print("[pi5_server] Renamed 'kaggle (1).json' to 'kaggle.json'.")
        except OSError as exc:
            print(f"[pi5_server] Failed to rename Kaggle credentials file: {exc}")

    if not os.path.exists(kaggle_json_path):
        return None, None
    try:
        with open(kaggle_json_path, "r", encoding="utf-8") as fh:
            payload = json.load(fh)
            username = payload.get("username")
            key = payload.get("key")
            if username and key:
                os.environ.setdefault("KAGGLE_USERNAME", username)
                os.environ.setdefault("KAGGLE_KEY", key)
                return username, key
    except (OSError, json.JSONDecodeError):
        pass
    return None, None


_kaggle_credentials()
_load_label_metadata()
_load_tflite_interpreter()


@app.post("/upload")
async def upload(request: Request) -> JSONResponse:
    global _latest_result

    img_bytes = await request.body()
    if not img_bytes:
        return JSONResponse({"status": "error", "message": "No image payload received"}, status_code=400)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"image_{timestamp}.jpg"
    filepath = os.path.join(UPLOAD_DIR, filename)
    with open(filepath, "wb") as file_obj:
        file_obj.write(img_bytes)

    analysis = _analyze_image(img_bytes)

    # Persist latest result for the polling endpoint and OLED display.
    _latest_result = {
        "timestamp": timestamp,
        "filename": filename,
        "path": filepath,
        "leaf_name": analysis["leaf_name"],
        "disease": analysis["disease"],
        "solution": analysis["solution"],
        "species": analysis["leaf_name"],
        "condition": analysis["disease"],
        "recommendation": analysis["solution"],
    }
    if metrics := analysis.get("metrics"):
        _latest_result["metrics"] = metrics

    response_payload = {
        "status": "success",
        "message": f"Image saved as {filename}",
        "size_bytes": len(img_bytes),
        **_latest_result,
    }
    return JSONResponse(response_payload)


@app.get("/result")
async def latest_result() -> JSONResponse:
    if _latest_result is None:
        return JSONResponse({"error": "No analysis available yet"}, status_code=404)
    return JSONResponse(_latest_result)
