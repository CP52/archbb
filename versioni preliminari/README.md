# Black-Box-Archery
ArchBB — Archery Black Box
A wearable shot analyzer for barebow and stringwalking archers, 
built on ESP32-S3 with IMU-based BLE data acquisition and a 
single-file HTML5 companion app.

# ArchBB is an open-source biomechanical shot analyzer for barebow archery. 
A compact ESP32-S3 module mounts on the bow riser and captures 6-axis IMU 
data (accelerometer + gyroscope) at 224 Hz across a 3-second window 
centered on the shot trigger. Data is streamed via BLE to a single-file 
HTML5 companion app running on any Android device with Chrome.

The app computes six shot metrics — Stability Score, Xi Consistency, 
Trigger Sharpness, Follow-through Score, Cant Angle, and Vibration Peak — 
and displays a 2D angular trace (pitch × roll in degrees) reconstructed 
via complementary filter. A real-time aiming sight with evanescent trail 
provides live feedback during the aiming phase. Session data is 
automatically saved to a timestamped CSV file.
