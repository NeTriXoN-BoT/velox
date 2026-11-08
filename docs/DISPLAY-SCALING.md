# Display scaling

Open **Display → Window scaling**, **System → Window scaling**, or right-click the phone toolbar and choose **Window scaling**. Changes apply immediately and are saved separately for each phone. [Deutsch](DISPLAY-SCALING.de.md).

| Setting | Meaning |
| :-- | :-- |
| Velox interface: Auto | Balances monitor DPI with desktop space. A normal 1080p or 1440p work area starts around 125%, and 4K around 150%, subject to the system's scaling and available space. Higher system accessibility scaling remains respected. |
| Velox interface: percentage | The 900 × 640 reference layout is redrawn at this physical-pixel percentage. Native input fields and Velox dialogs follow the effective size. |
| Phone window: Auto | Fits the phone and toolbar within the desktop, leaving space around them and limiting automatic sizing on large or ultrawide displays. Refits after rotation, monitor/DPI changes, or toolbar docking changes. |
| Phone window: percentage | The displayed Android render area as a percentage of its native render resolution; for example, 25% of 1080 × 2400 is 270 × 600 pixels before the frame. |

Auto is the default for both. If a requested percentage exceeds the usable desktop, Velox applies a smaller percentage and displays the actual value. The taskbar, window caption, borders and toolbar are included in the fit. The saved request is retained, so it can fit on a larger screen later. Valid custom percentages already saved in settings also appear in the choices.

You can still move or resize the phone manually. Velox does not continuously reset its geometry. Full-screen and maximized phone windows retain their own behavior. Closing controls leaves the phone running at its current size.

Scaling changes the desktop presentation. It does not change Android resolution/density, game render quality, refresh rate, memory or CPU allocations, and does not require a phone restart. Android resolution remains under **Display → Resolution** and the phone toolbar's **Screen resolution** action.

For a reset, choose **Auto** in both rows. On a disconnected or smaller monitor, the launcher is brought back within the available desktop when it receives the display-change notification.
