import io
import os
from datetime import datetime
from typing import Dict, Optional

import torch
from flask import Flask, jsonify, request
from PIL import Image

app = Flask(__name__)

# Directory where incoming images are stored so the ESP32 can fetch results later.
UPLOAD_DIR = os.path.join(os.path.dirname(__file__), "upload")
os.makedirs(UPLOAD_DIR, exist_ok=True)

# Cache the latest analysis so the ESP32 hub can poll /result and refresh its OLED.
_latest_result: Optional[Dict[str, object]] = None


def _analyze_image(img_bytes: bytes) -> Dict[str, object]:
    """Very lightweight heuristic analysis to provide demo feedback."""
    with Image.open(io.BytesIO(img_bytes)) as pil_img:
        pil_img = pil_img.convert("RGB")
        # torch.tensor from list keeps dependencies minimal (no numpy required).
        data = torch.tensor(list(pil_img.getdata()), dtype=torch.float32) / 255.0
        if data.numel() == 0:
            return {
                "leaf_name": "Unknown leaf",
                "disease": "No image data",
                "solution": "Re-capture the image.",
                "metrics": {}
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
            },
        }


@app.route("/upload", methods=["POST"])
def upload() -> tuple:
    global _latest_result

    img_bytes = request.get_data()
    if not img_bytes:
        return jsonify({"status": "error", "message": "No image payload received"}), 400

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
    return jsonify(response_payload)


@app.route("/result", methods=["GET"])
def latest_result() -> tuple:
    if _latest_result is None:
        return jsonify({"error": "No analysis available yet"}), 404
    return jsonify(_latest_result)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
