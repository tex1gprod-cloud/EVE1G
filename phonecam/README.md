# PhoneCam

A small side experiment, separate from EVE1G: watch an Android phone camera on the PC and record it there, over Wi-Fi.

The phone runs the **IP Webcam** app. PhoneCam connects to it by IP address, shows the live picture in your browser and saves recordings to the PC. It can also drive the phone's camera.

## What you need

- The phone and the PC on the **same Wi-Fi**.
- **IP Webcam** on the phone (Google Play). Open it, scroll down, tap **Start server**. The address is shown at the bottom of the phone screen, for example `http://192.168.1.23:8080`.
- **Python 3.9+** on the PC ([python.org](https://www.python.org/downloads/), tick "Add Python to PATH" when installing).

## Start it

**Windows:** double-click `start.bat`. The first time it installs OpenCV and asks for the phone address. It remembers the address, so after that a double-click is all you need.

**Command line:**

```
python -m pip install -r requirements.txt
python phonecam.py 192.168.1.23
```

The viewer opens in your browser at http://localhost:8090.

## Watch from other devices on the Wi-Fi

`start.bat` opens the viewer to your home network. The window prints an address such as:

```
On the Wi-Fi: http://192.168.1.40:8090
```

Open that address on any phone, tablet or PC on the same Wi-Fi. The first time, Windows asks whether to let Python through the firewall: click **Allow** on **Private networks**.

From the command line, add `--host 0.0.0.0` to do the same. Without it, the viewer works only on the PC that runs PhoneCam.

Anyone on your Wi-Fi who opens that address can see the camera and press the buttons. That's fine at home, but don't run it this way on public Wi-Fi.

If you set a login in IP Webcam (Settings > Local broadcasting > Login/password):

```
python phonecam.py 192.168.1.23 --user dad --password secret
```

To use a different phone address later, pass it on the command line or delete `phonecam.json`.

## What it can do

| Button | What happens |
| --- | --- |
| Start / Stop recording | Records the live picture to an `.mp4` on the PC, in `recordings/`. The video plays at real speed even when the Wi-Fi drops frames. |
| Take snapshot | Saves the current picture as a `.jpg` in `recordings/`. |
| Torch, Focus, Zoom | Controls the phone's flashlight, autofocus and zoom. |
| Back / Front camera | Switches the phone's camera. |
| Quality, Resolution | Changes what the phone sends. A higher setting looks better but needs more Wi-Fi. |
| Record on the phone | Starts or stops IP Webcam's own recording, saved on the phone with sound. |

Files in "Saved on this PC" can be downloaded by clicking them. They are also in the `recordings` folder next to the script.

## Notes

- PC recordings have **no sound**. IP Webcam's audio is a separate stream. Use "Record on the phone" if you need sound.
- Keep the phone screen on, or enable IP Webcam's "Prevent sleep" option. Otherwise Android may pause the stream. If the stream drops, PhoneCam reconnects by itself.
- Closing PhoneCam during a recording still saves the video properly.
- `python phonecam.py --help` lists all options (port, recording folder, fixed frame rate).
