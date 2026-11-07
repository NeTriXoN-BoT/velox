# Einen Velox-Download vor dem Öffnen prüfen

[English](VERIFY-DOWNLOADS.md) · [Offizielle Downloads](https://github.com/NeTriXoN-BoT/velox/releases/latest)

## Warum Windows eine Warnung anzeigen kann

Der Windows-Installer und `Velox.exe` haben derzeit **keine Authenticode-Herausgebersignatur**. Windows kann **Unbekannter Herausgeber** oder die SmartScreen-Meldung **Der Computer wurde durch Windows geschützt** anzeigen. Bei diesen nicht signierten Versionen ist das zu erwarten. Die Warnung allein sagt aber nicht aus, ob eine Datei unbedenklich ist. Der signierte Aktualisierungskanal von Velox ist unabhängig von der Windows-Herausgeberprüfung.

## 1. Quelle und Version kontrollieren

Verwende die Release-Seite unter **https://github.com/NeTriXoN-BoT/velox/releases**. Prüfe vor dem Download den Kontonamen und den Namen des Repositorys.

Für Version **2.4.0** wählst du `Velox-Setup-2.4.0.exe` oder `Velox-2.4.0.zip`. Lade außerdem **SHA256SUMS.txt aus genau diesem Release** herunter. Öffne diese Textdatei im Editor. Jede Zeile enthält einen SHA-256-Wert und den zugehörigen Dateinamen. Für Installer und ZIP kannst du stattdessen auch die jeweils passende `.sha256`-Datei verwenden.

Bei einer anderen Version verwendest du deren Dateinamen und Prüfsummendatei. Die automatisch von GitHub angebotenen **Source code**-Archive enthalten den Quellcode zum Bauen des Projekts und sind kein portables App-Paket.

## 2. SHA-256 der heruntergeladenen Datei berechnen

Öffne den Downloadordner im Datei-Explorer. Gib oben in die Adressleiste `powershell` ein und drücke die Eingabetaste. PowerShell öffnet sich dann in diesem Ordner. Administratorrechte sind nicht erforderlich. Liegt die Datei woanders, trage ihren tatsächlichen Pfad innerhalb der Anführungszeichen ein.

Für den **Installer**:

```powershell
Get-FileHash -LiteralPath '.\Velox-Setup-2.4.0.exe' -Algorithm SHA256 | Format-List Algorithm,Hash
```

Für die **portable ZIP**:

```powershell
Get-FileHash -LiteralPath '.\Velox-2.4.0.zip' -Algorithm SHA256 | Format-List Algorithm,Hash
```

Führe nur den Befehl für deine heruntergeladene Datei aus. Diese Befehle lesen die Datei, ohne Installer oder App zu starten. `Format-List` zeigt die vollständige Prüfsumme an, statt sie in einer Tabellenspalte abzukürzen.

Vergleiche **alle 64 Zeichen** unter `Hash` mit der Zeile für genau diesen Dateinamen in der `SHA256SUMS.txt` des Releases. Groß- und Kleinschreibung sind dabei egal.

- **Übereinstimmung:** Die heruntergeladene Datei stimmt mit der veröffentlichten Prüfsumme überein.
- **Abweichung oder fehlender Eintrag:** Brich ab und starte weder Installer noch App. Lade die Datei erneut aus dem offiziellen Release herunter. Bleibt die Abweichung bestehen, [melde sie](https://github.com/NeTriXoN-BoT/velox/issues).
- **Datei nicht gefunden:** Kontrolliere Ordner und Dateinamen. Wurde der Download etwa mit `(1)` im Namen gespeichert, musst du diesen tatsächlichen Namen im Befehl verwenden.

Die Prüfsumme bestätigt die Übereinstimmung mit der veröffentlichten Datei. Sie ist kein Virenscan, keine unabhängige Herausgeberprüfung und kein Beweis für die Unbedenklichkeit einer Software. Beziehe Datei und Prüfsumme vom offiziellen Projekt; eine fremde Downloadseite, die beides liefert, reicht als Vertrauensnachweis nicht aus.

Prüfe die portable ZIP **vor dem Entpacken** und entpacke sie anschließend in einen eigenen Ordner. Die ZIP enthält eine weitere `SHA256SUMS.txt` für die entpackten Dateien. Du kannst dort zusätzlich `Get-FileHash -LiteralPath '.\Velox.exe' -Algorithm SHA256 | Format-List Algorithm,Hash` ausführen und das Ergebnis mit dem Eintrag für `Velox.exe` in dieser enthaltenen Prüfsummendatei vergleichen.

## 3. Bei einer Windows-Warnung entscheiden

Nach der Quellen- und Prüfsummenprüfung öffnest du den Installer oder bei der portablen Variante die entpackte `Velox.exe`. Wenn du dem Projekt vertraust und SmartScreen vor einer unbekannten App warnt, kannst du **Weitere Informationen → Trotzdem ausführen** wählen, **sofern Windows diese Optionen anbietet**. Die Anzeige **Unbekannter Herausgeber** bleibt dabei zu erwarten; eine Prüfsummenprüfung ändert sie nicht.

Deaktiviere weder Microsoft Defender noch SmartScreen oder Smart App Control, um Velox zu installieren. Gibt es keine Möglichkeit zum Fortfahren, kann eine Sicherheits- oder Administratorrichtlinie die nicht signierte Datei blockieren. Beachte diese Einschränkung und wende dich bei einem verwalteten PC an die Administration. Eine konkrete Virenerkennung unterscheidet sich von einer Warnung vor einem unbekannten Herausgeber: Brich in diesem Fall ab und melde die Erkennung zur Prüfung, statt eine Ausnahme hinzuzufügen.

Microsoft-Dokumentation: [SmartScreen und nicht signierte Dateien](https://learn.microsoft.com/de-de/windows/apps/package-and-deploy/smartscreen-reputation) · [PowerShell Get-FileHash](https://learn.microsoft.com/de-de/powershell/module/microsoft.powershell.utility/get-filehash).
