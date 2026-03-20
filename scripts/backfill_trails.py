"""
Backfill the Trail table from existing DetectionFrame/DetectionBox data.
Replicates the same logic as ClipReprocessWorker::ComputeAndStoreTrails.

Usage:
    python scripts/backfill_trails.py [path_to_server.db]

If no path is given, defaults to %%ProgramData%%\\Witness\\server.db
"""

import sqlite3
import sys
import os
import math
import json
import time

# --- Constants matching C++ ---
MAX_TIME_GAP = 5.0       # seconds
MAX_SPATIAL_JUMP = 0.25   # normalized coords
RDP_EPSILON = 0.008       # simplification tolerance


def simplify_rdp(pts: list[list[float]], epsilon: float) -> list[list[float]]:
    """Ramer-Douglas-Peucker simplification. pts = [[x, y, t], ...]"""
    if len(pts) <= 2:
        return pts

    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]
    len_sq = dx * dx + dy * dy

    max_dist = 0.0
    max_idx = 0
    for i in range(1, len(pts) - 1):
        p = pts[i]
        if len_sq == 0:
            dist = math.sqrt((p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2)
        else:
            t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len_sq))
            px, py = a[0] + t * dx, a[1] + t * dy
            dist = math.sqrt((p[0] - px) ** 2 + (p[1] - py) ** 2)
        if dist > max_dist:
            max_dist = dist
            max_idx = i

    if max_dist > epsilon:
        left = simplify_rdp(pts[:max_idx + 1], epsilon)
        right = simplify_rdp(pts[max_idx:], epsilon)
        return left[:-1] + right
    return [a, b]


def filter_outliers(pts: list[list[float]]) -> list[list[float]]:
    """Remove spikey jumps using 90th percentile threshold."""
    if len(pts) < 4:
        return pts

    dists = []
    for i in range(1, len(pts)):
        d = math.sqrt((pts[i][0] - pts[i-1][0]) ** 2 + (pts[i][1] - pts[i-1][1]) ** 2)
        dists.append(d)

    sorted_dists = sorted(dists)
    p90 = sorted_dists[int(len(sorted_dists) * 0.9)]
    threshold = max(p90 * 2.5, 0.01)

    filtered = [pts[0]]
    for i in range(1, len(pts)):
        if dists[i-1] <= threshold:
            filtered.append(pts[i])
    return filtered


def split_on_discontinuities(points: list[list[float]]) -> list[list[list[float]]]:
    """Split trail points into segments on temporal/spatial gaps."""
    if len(points) < 2:
        return [points]

    segments = []
    current = [points[0]]

    for i in range(1, len(points)):
        prev, curr = points[i - 1], points[i]
        dt = curr[2] - prev[2]
        dist = math.sqrt((curr[0] - prev[0]) ** 2 + (curr[1] - prev[1]) ** 2)

        if dt > MAX_TIME_GAP or dist > MAX_SPATIAL_JUMP:
            if len(current) >= 2:
                segments.append(current)
            current = []
        current.append(curr)

    if len(current) >= 2:
        segments.append(current)
    return segments


def compact_json(pts: list[list[float]]) -> str:
    """Serialize points as compact JSON: [[x,y,t],...]"""
    parts = []
    for p in pts:
        parts.append(f"[{p[0]:.6f},{p[1]:.6f},{p[2]:.2f}]")
    return "[" + ",".join(parts) + "]"


def backfill(db_path: str):
    if not os.path.exists(db_path):
        print(f"ERROR: Database not found at {db_path}")
        sys.exit(1)

    print(f"Opening database: {db_path}")
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")

    # Ensure Trail table exists
    conn.execute("""
        CREATE TABLE IF NOT EXISTS Trail(
            TrailUID  INTEGER PRIMARY KEY AUTOINCREMENT,
            ClipUID   INTEGER NOT NULL,
            CameraID  INTEGER NOT NULL,
            ClassName TEXT    NOT NULL,
            FaceName  TEXT,
            StartTime REAL    NOT NULL,
            EndTime   REAL    NOT NULL,
            PointData TEXT    NOT NULL,
            FOREIGN KEY(ClipUID) REFERENCES Clip(ClipUID) ON DELETE CASCADE
        )
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS idx_trail_camera_time ON Trail(CameraID, StartTime, EndTime)")
    conn.commit()

    # Get all clips that have detection data (DetectionVersion > 0)
    clips = conn.execute("""
        SELECT ClipUID, Timestamp, Camera, Duration
        FROM Clip
        WHERE DetectionVersion > 0
        ORDER BY Timestamp DESC
    """).fetchall()

    print(f"Found {len(clips)} clips with detection data")

    # Check which clips already have trails
    existing = set(r[0] for r in conn.execute("SELECT DISTINCT ClipUID FROM Trail").fetchall())
    to_process = [(c[0], c[1], c[2], c[3]) for c in clips if c[0] not in existing]
    print(f"  {len(existing)} already have trails, {len(to_process)} to backfill")

    if not to_process:
        print("Nothing to do!")
        conn.close()
        return

    total_trails = 0
    t0 = time.time()

    for idx, (clip_uid, timestamp, camera, duration) in enumerate(to_process):
        duration_sec = float(duration or 0)
        from_time = float(timestamp)
        to_time = from_time + duration_sec

        # Fetch detection boxes for this clip
        rows = conn.execute("""
            SELECT f.Timestamp, b.TrackingID, b.ClassName, b.X, b.Y, b.W, b.H, kf.Name
            FROM DetectionFrame f
            LEFT JOIN DetectionBox b ON f.FrameUID = b.FrameUID
            LEFT JOIN FaceCrop fc ON fc.FrameUID = b.FrameUID AND fc.TrackingID = b.TrackingID
            LEFT JOIN FaceEmbedding fe ON fe.FaceCropUID = fc.CropUID AND fe.KnownFaceUID IS NOT NULL
            LEFT JOIN KnownFace kf ON kf.KnownFaceUID = fe.KnownFaceUID
            WHERE f.CameraID = ? AND f.Timestamp >= ? AND f.Timestamp <= ?
            ORDER BY f.Timestamp ASC, b.BoxUID ASC
        """, (camera, from_time, to_time)).fetchall()

        if not rows:
            continue

        # Group by (TrackingID, ClassName)
        trail_map: dict[str, dict] = {}
        for ts, track_id, cls_name, x, y, w, h, face_name in rows:
            if not cls_name:
                continue
            # Bottom-center anchor
            ax = x + w / 2.0
            ay = y + h

            key = f"{track_id}:{cls_name}"
            if key not in trail_map:
                trail_map[key] = {"cls": cls_name, "face": None, "points": []}
            trail = trail_map[key]
            if face_name and not trail["face"]:
                trail["face"] = face_name
            trail["points"].append([ax, ay, ts])

        # Split, simplify, store
        clip_trails = 0
        for key, trail in trail_map.items():
            pts = trail["points"]
            if len(pts) < 2:
                continue

            pts.sort(key=lambda p: p[2])
            pts = filter_outliers(pts)
            if len(pts) < 2:
                continue
            segments = split_on_discontinuities(pts)

            for seg in segments:
                simplified = simplify_rdp(seg, RDP_EPSILON)
                if len(simplified) < 2:
                    continue

                start_time = simplified[0][2]
                end_time = simplified[-1][2]
                point_data = compact_json(simplified)

                conn.execute(
                    "INSERT INTO Trail (ClipUID, CameraID, ClassName, FaceName, StartTime, EndTime, PointData) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (clip_uid, camera, trail["cls"], trail["face"], start_time, end_time, point_data)
                )
                clip_trails += 1

        total_trails += clip_trails

        if (idx + 1) % 50 == 0 or idx == len(to_process) - 1:
            conn.commit()
            elapsed = time.time() - t0
            rate = (idx + 1) / elapsed if elapsed > 0 else 0
            remaining = (len(to_process) - idx - 1) / rate if rate > 0 else 0
            print(f"  [{idx + 1}/{len(to_process)}] {total_trails} trails generated "
                  f"({rate:.1f} clips/s, ~{remaining:.0f}s remaining)")

    conn.commit()
    elapsed = time.time() - t0
    print(f"\nDone! Generated {total_trails} trails from {len(to_process)} clips in {elapsed:.1f}s")
    conn.close()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        db_path = sys.argv[1]
    else:
        program_data = os.environ.get("ProgramData", r"C:\ProgramData")
        db_path = os.path.join(program_data, "Witness", "server.db")

    backfill(db_path)
