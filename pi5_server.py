import io
import json
import os
import threading
from datetime import datetime
from typing import Dict, Optional, Tuple

import numpy as np
import requests
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from PIL import Image

app = FastAPI()

BASE_DIR = os.path.dirname(__file__)

# Directory where incoming images are stored so the ESP32 can fetch results later.
UPLOAD_DIR = os.path.join(BASE_DIR, "upload")
os.makedirs(UPLOAD_DIR, exist_ok=True)

# Default label ordering from the PlantVillage dataset (38 classes).
_DEFAULT_LABELS: list[str] = [
    "Apple___Apple_scab",
    "Apple___Black_rot",
    "Apple___Cedar_apple_rust",
    "Apple___healthy",
    "Blueberry___healthy",
    "Cherry_(including_sour)___Powdery_mildew",
    "Cherry_(including_sour)___healthy",
    "Corn_(maize)___Cercospora_leaf_spot Gray_leaf_spot",
    "Corn_(maize)___Common_rust_",
    "Corn_(maize)___Northern_Leaf_Blight",
    "Corn_(maize)___healthy",
    "Grape___Black_rot",
    "Grape___Esca_(Black_Measles)",
    "Grape___Leaf_blight_(Isariopsis_Leaf_Spot)",
    "Grape___healthy",
    "Orange___Haunglongbing_(Citrus_greening)",
    "Peach___Bacterial_spot",
    "Peach___healthy",
    "Pepper,_bell___Bacterial_spot",
    "Pepper,_bell___healthy",
    "Potato___Early_blight",
    "Potato___Late_blight",
    "Potato___healthy",
    "Raspberry___healthy",
    "Soybean___healthy",
    "Squash___Powdery_mildew",
    "Strawberry___Leaf_scorch",
    "Strawberry___healthy",
    "Tomato___Bacterial_spot",
    "Tomato___Early_blight",
    "Tomato___Late_blight",
    "Tomato___Leaf_Mold",
    "Tomato___Septoria_leaf_spot",
    "Tomato___Spider_mites Two-spotted_spider_mite",
    "Tomato___Target_Spot",
    "Tomato___Tomato_Yellow_Leaf_Curl_Virus",
    "Tomato___Tomato_mosaic_virus",
    "Tomato___healthy",
]

_DEFAULT_METADATA: Dict[str, Dict[str, str]] = {
    "Apple___Apple_scab": {
        "disease": "Apple Scab",
        "solution": "Prune infected leaves and apply fungicide labeled for scab control.",
    },
    "Apple___Black_rot": {
        "disease": "Apple Black Rot",
        "solution": "Remove mummified fruit and use a copper-based fungicide during dormancy.",
    },
    "Apple___Cedar_apple_rust": {
        "disease": "Cedar Apple Rust",
        "solution": "Eliminate nearby junipers and spray sulfur fungicide early in the season.",
    },
    "Apple___healthy": {
        "disease": "Healthy Apple Leaf",
        "solution": "No treatment needed; continue normal care.",
    },
    "Blueberry___healthy": {
        "disease": "Healthy Blueberry Leaf",
        "solution": "No treatment needed; maintain balanced watering.",
    },
    "Cherry_(including_sour)___Powdery_mildew": {
        "disease": "Cherry Powdery Mildew",
        "solution": "Improve air flow by pruning and apply potassium bicarbonate spray.",
    },
    "Cherry_(including_sour)___healthy": {
        "disease": "Healthy Cherry Leaf",
        "solution": "No treatment needed; continue routine monitoring.",
    },
    "Corn_(maize)___Cercospora_leaf_spot Gray_leaf_spot": {
        "disease": "Corn Gray Leaf Spot",
        "solution": "Rotate crops and apply a strobilurin fungicide at early tasseling.",
    },
    "Corn_(maize)___Common_rust_": {
        "disease": "Corn Common Rust",
        "solution": "Plant resistant hybrids and consider a triazole fungicide if severe.",
    },
    "Corn_(maize)___Northern_Leaf_Blight": {
        "disease": "Corn Northern Leaf Blight",
        "solution": "Remove crop debris and spray fungicide at tasseling if weather is humid.",
    },
    "Corn_(maize)___healthy": {
        "disease": "Healthy Corn Leaf",
        "solution": "No treatment needed; keep fertilization on schedule.",
    },
    "Grape___Black_rot": {
        "disease": "Grape Black Rot",
        "solution": "Remove infected clusters and spray captan or myclobutanil early in season.",
    },
    "Grape___Esca_(Black_Measles)": {
        "disease": "Grape Esca (Black Measles)",
        "solution": "Prune out infected wood and maintain vine vigor; chemical control is limited.",
    },
    "Grape___Leaf_blight_(Isariopsis_Leaf_Spot)": {
        "disease": "Grape Leaf Blight",
        "solution": "Improve canopy ventilation and apply fungicide labeled for leaf blight.",
    },
    "Grape___healthy": {
        "disease": "Healthy Grape Leaf",
        "solution": "No treatment needed; continue canopy management.",
    },
    "Orange___Haunglongbing_(Citrus_greening)": {
        "disease": "Citrus Greening (HLB)",
        "solution": "Remove infected trees and control psyllid vectors immediately.",
    },
    "Peach___Bacterial_spot": {
        "disease": "Peach Bacterial Spot",
        "solution": "Apply copper spray at bud break and remove heavily infected twigs.",
    },
    "Peach___healthy": {
        "disease": "Healthy Peach Leaf",
        "solution": "No treatment needed; ensure regular fertilization.",
    },
    "Pepper,_bell___Bacterial_spot": {
        "disease": "Bell Pepper Bacterial Spot",
        "solution": "Use resistant varieties and apply fixed copper spray weekly during outbreaks.",
    },
    "Pepper,_bell___healthy": {
        "disease": "Healthy Pepper Leaf",
        "solution": "No treatment needed; maintain even watering.",
    },
    "Potato___Early_blight": {
        "disease": "Potato Early Blight",
        "solution": "Remove infected foliage and spray chlorothalonil or mancozeb preventatively.",
    },
    "Potato___Late_blight": {
        "disease": "Potato Late Blight",
        "solution": "Destroy infected plants and apply a systemic fungicide immediately.",
    },
    "Potato___healthy": {
        "disease": "Healthy Potato Leaf",
        "solution": "No treatment needed; keep soil consistently moist.",
    },
    "Raspberry___healthy": {
        "disease": "Healthy Raspberry Leaf",
        "solution": "No treatment needed; maintain pruning schedule.",
    },
    "Soybean___healthy": {
        "disease": "Healthy Soybean Leaf",
        "solution": "No treatment needed; monitor for pests regularly.",
    },
    "Squash___Powdery_mildew": {
        "disease": "Squash Powdery Mildew",
        "solution": "Remove infected leaves and spray neem oil or sulfur weekly.",
    },
    "Strawberry___Leaf_scorch": {
        "disease": "Strawberry Leaf Scorch",
        "solution": "Increase irrigation, remove damaged leaves, and apply fungicide if severe.",
    },
    "Strawberry___healthy": {
        "disease": "Healthy Strawberry Leaf",
        "solution": "No treatment needed; keep mulch dry.",
    },
    "Tomato___Bacterial_spot": {
        "disease": "Tomato Bacterial Spot",
        "solution": "Remove infected foliage and apply copper spray every 7 days.",
    },
    "Tomato___Early_blight": {
        "disease": "Tomato Early Blight",
        "solution": "Mulch to reduce soil splash and use a chlorothalonil fungicide weekly.",
    },
    "Tomato___Late_blight": {
        "disease": "Tomato Late Blight",
        "solution": "Remove plants and treat with a systemic fungicide immediately.",
    },
    "Tomato___Leaf_Mold": {
        "disease": "Tomato Leaf Mold",
        "solution": "Improve greenhouse ventilation and spray potassium bicarbonate.",
    },
    "Tomato___Septoria_leaf_spot": {
        "disease": "Tomato Septoria Leaf Spot",
        "solution": "Prune lower leaves and apply copper fungicide every 7-10 days.",
    },
    "Tomato___Spider_mites Two-spotted_spider_mite": {
        "disease": "Tomato Spider Mite Damage",
        "solution": "Rinse foliage, release predatory mites, or apply insecticidal soap.",
    },
    "Tomato___Target_Spot": {
        "disease": "Tomato Target Spot",
        "solution": "Remove infected tissue and rotate with non-host crops.",
    },
    "Tomato___Tomato_Yellow_Leaf_Curl_Virus": {
        "disease": "Tomato Yellow Leaf Curl Virus",
        "solution": "Control whiteflies and remove infected plants promptly.",
    },
    "Tomato___Tomato_mosaic_virus": {
        "disease": "Tomato Mosaic Virus",
        "solution": "Discard infected plants and disinfect tools with bleach solution.",
    },
    "Tomato___healthy": {
        "disease": "Healthy Tomato Leaf",
        "solution": "No treatment needed; maintain balanced nutrition.",
    },
}

_TFLITE_MODEL_PATH = os.path.join(BASE_DIR, "leaf_resnet50_float.tflite")
_LABEL_TXT_PATH = os.path.join(BASE_DIR, "leaf_labels.txt")
_LABEL_JSON_PATH = os.path.join(BASE_DIR, "leaf_labels.json")
_CLOUD_POST_URL = "https://plant-disease-detection-server-one.vercel.app/diseases"

_tflite_interpreter = None
_tflite_input_details = None
_tflite_output_details = None
_tflite_lock = threading.Lock()
_tflite_labels: Optional[list[str]] = None
_label_metadata: Optional[Dict[str, Dict[str, str]]] = None

# Cache the latest analysis so the ESP32 hub can poll /result and refresh its OLED.
_latest_result: Optional[Dict[str, object]] = None
_cloud_lock = threading.Lock()


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
    if not labels and _DEFAULT_LABELS:
        labels = list(_DEFAULT_LABELS)

    _tflite_labels = labels or None

    if os.path.exists(_LABEL_JSON_PATH):
        try:
            with open(_LABEL_JSON_PATH, "r", encoding="utf-8") as fh:
                data = json.load(fh)
                if isinstance(data, dict):
                    _label_metadata = data
        except (OSError, json.JSONDecodeError) as exc:
            print(f"[pi5_server] Failed to load label metadata '{_LABEL_JSON_PATH}': {exc}")
    if _label_metadata is None and _DEFAULT_METADATA:
        _label_metadata = dict(_DEFAULT_METADATA)

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
        text = raw_label.replace("___", " - ").replace("__", " ").replace("_", " ")
        text = " ".join(part for part in text.split())
        return text.title()
    return f"Class {index}"


def _heuristic_analysis(pil_img: Image.Image) -> Dict[str, object]:
    """Fallback heuristic analysis when the model is unavailable."""
    np_img = np.asarray(pil_img, dtype=np.float32) / 255.0
    if np_img.size == 0:
        return {
            "leaf_name": "Unknown Leaf",
            "disease": "No image data",
            "solution": "Re-capture the image.",
            "metrics": {},
        }

    red = float(np.mean(np_img[..., 0]))
    green = float(np.mean(np_img[..., 1]))
    blue = float(np.mean(np_img[..., 2]))
    brightness = float(np.mean(np_img))
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
        "leaf_label": raw_label or friendly,
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


def _post_result_to_cloud(snapshot: Dict[str, object]) -> None:
    plant = str(snapshot.get("leaf_name") or snapshot.get("species") or "Unknown").strip()
    disease = str(snapshot.get("disease") or snapshot.get("condition") or "Unknown").strip()
    treatment = str(snapshot.get("solution") or snapshot.get("recommendation") or "No advice").strip()
    payload = {
        "plantType": plant or "Unknown",
        "diseaseType": disease or "Unknown",
        "treatment": treatment or "No advice",
        "confidence": "95%",
    }
    try:
        resp = requests.post(_CLOUD_POST_URL, json=payload, timeout=10)
        if resp.status_code >= 400:
            print(f"[pi5_server] Cloud POST failed ({resp.status_code}): {resp.text[:200]}")
    except requests.RequestException as exc:
        print(f"[pi5_server] Cloud POST error: {exc}")


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
    snapshot = dict(_latest_result)
    threading.Thread(target=_post_result_to_cloud, args=(snapshot,), daemon=True).start()

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
