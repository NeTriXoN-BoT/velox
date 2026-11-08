# Fensterskalierung

Öffne **Anzeige → Fensterskalierung**, **System → Fensterskalierung** oder klicke mit der rechten Maustaste auf die Handy-Symbolleiste und wähle **Fensterskalierung**. Änderungen gelten sofort und werden für jedes Handy getrennt gespeichert. [English](DISPLAY-SCALING.md).

| Einstellung | Bedeutung |
| :-- | :-- |
| Velox-Oberfläche: Auto | Stimmt Windows-DPI und den Platz auf dem Desktop ab. Ein normaler 1080p-Arbeitsbereich startet ungefähr bei 125 %, 1440p bei 150 % und 4K bei 200 %. Windows-Skalierung und verfügbarer Platz werden berücksichtigt. |
| Velox-Oberfläche: Prozentwert | Die Referenzoberfläche von 900 × 640 wird mit diesem Verhältnis in physischen Pixeln neu gezeichnet. Eingabefelder und Velox-Dialoge folgen der tatsächlich angewendeten Größe. |
| Handyfenster: Auto | Passt Handy und Symbolleiste mit etwas Abstand in den Desktop ein. Nach Drehung, Bildschirm-/DPI-Wechsel oder einem Wechsel der Andockseite wird die Größe erneut angepasst. |
| Handyfenster: Prozentwert | Größe des sichtbaren Android-Bildes im Verhältnis zur Android-Auflösung. 25 % von 1080 × 2400 entsprechen zum Beispiel 270 × 600 Pixeln ohne Rahmen. |

Beide Einstellungen stehen standardmäßig auf Auto. Passt der gewählte Prozentwert nicht auf den Desktop, verwendet Velox einen kleineren Wert und zeigt diesen an. Taskleiste, Fenstertitel, Rahmen und Symbolleiste werden berücksichtigt. Die Auswahl bleibt gespeichert und kann auf einem größeren Bildschirm wieder verwendet werden. Bereits gespeicherte gültige benutzerdefinierte Werte erscheinen ebenfalls in der Auswahl.

Das Handy lässt sich weiterhin manuell verschieben und skalieren. Velox setzt seine Größe nicht ständig zurück. Vollbild und maximierte Handyfenster behalten ihr eigenes Verhalten. Beim Schließen der Steuerung läuft das Handy in seiner aktuellen Größe weiter.

Die Skalierung ändert die Darstellung auf dem Desktop. Android-Auflösung/-Dichte, Spielqualität, Bildrate, Arbeitsspeicher und CPU-Zuweisung bleiben unabhängig. Ein Handy-Neustart ist nicht nötig. Die Android-Auflösung lässt sich unter **Anzeige → Auflösung** oder über **Bildschirmauflösung** in der Handy-Symbolleiste ändern.

Zum Zurücksetzen wähle in beiden Zeilen **Auto**. Nach dem Trennen eines Bildschirms oder dem Wechsel auf einen kleineren Bildschirm bringt Velox das Hauptfenster bei der entsprechenden Windows-Benachrichtigung in den verfügbaren Desktop zurück.
