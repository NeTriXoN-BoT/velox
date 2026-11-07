# Deine Handys, deine Einstellungen

[English](PHONE-MANAGEMENT.md) · [Zurück zu Velox](../README.de.md)

Öffne **Instanzen → Verwalten** beim gewünschten Handy. Name, Speicher und Kopien gehören zu diesem Handy; das gemeinsame Android-SDK bleibt getrennt.

### Name, Vorlage oder vollständiger Klon

**Umbenennen** ändert den sichtbaren Namen, ohne die Speicherkennung anzutasten. Eine **frische Vorlage** erstellt neue Android-Daten und übernimmt Einstellungen, Steuerungen für beide Ausrichtungen, Controller-Konfiguration und Spielprofile. Konten und installierte Apps des Originals werden nicht übernommen.

Ein **vollständiger Klon** kopiert die Datenträger des ausgeschalteten Handys, prüft ihre Prüfsummen und passt die Verweise auf Basisabbilder an. Er enthält private Apps, Konten und Spielstände. Bewahre ihn vertraulich auf. Manche Apps verlangen eine neue Anmeldung; Spiele können die gleichzeitige Nutzung desselben Kontos einschränken. Für ein anderes Konto eignet sich eine frische Vorlage.

Schalte das Quellhandy aus und schließe sein anderes Kontrollfenster vor dem Kopieren. Kopien starten vollständig neu und zunächst ohne automatischen Start. Maximal ein Haupthandy und acht zusätzliche Handys sind möglich; gleichzeitige Nutzung benötigt ausreichend CPU und RAM. RAM-Werte bleiben Gast-Zielwerte. Das Budget reserviert mindestens 3 GB pro aktivem Handy für das Minimum des verwendeten Android-Abbilds; Prozess- und Grafikbedarf kommen hinzu.

### Speicher mit Rückweg

**Speicher** zeigt die logische Größe der Handydateien, belegten Datenträgerplatz, Desktop-Daten und bekannte temporäre Dateien. Gemeinsames SDK und Tresorsicherungen zählen nicht zum Handyspeicher. Sparse-Dateien können weniger Platz belegen, als ihre logische Größe angibt.

**Bereinigung prüfen** zeigt die bekannten Ziele vor dem Verschieben von Snapshots, Cache und Protokollen in den Windows-Papierkorb. App- und Spielstanddatenträger bleiben erhalten. Entferne bei Bedarf genau diese Papierkorbelemente selbst, um Platz freizugeben.

**Laufwerk wählen** erstellt und prüft eine neue Handykopie auf einem lokalen festen Laufwerk, passt Datenträgerverweise an und wechselt erst nach Erfolg. Das Original bleibt erhalten. **Verschieben rückgängig** wählt dessen früheren Zustand; neuere Änderungen bleiben in der verschobenen Kopie. **Vorherige Kopie** öffnet den ursprünglichen AVD-Ordner. Teste die neue Kopie, bevor du den alten Ordner selbst verwaltest. Behalte das gemeinsame SDK und andere Handys. Bei einem Abbruch bleibt das Original erhalten; Velox nennt den Speicherort der unvollständigen Ausgabe.

### Steuerung teilen, Daten behalten

Öffne **Steuerung → Spielsteuerung teilen**. Der Export enthält gespeicherte Hoch-/Querformat-Belegungen, Controller-Einstellungen und das Spielprofil in einer lesbaren `.velox-controls`-Datei. Konten, Spielstände, lokale SDK-Pfade und Controller-Geräte-IDs sind nicht enthalten.

Vor dem Import wird das Android-Paket angezeigt. Ersetzt wird nur dessen gespeicherte Spielsteuerung. Nicht verfügbare CPU- oder RAM-Werte verwenden die globalen Einstellungen. Vorherige Dateien bleiben in `data/layout-history`; **Letzten Steuerungsimport rückgängig machen** stellt sie wieder her. Ein unterbrochener Import wird beim erneuten Öffnen zurückgerollt. Schließe Control Studio vor Import und Rückgängig.

### Ruhigere Hintergrund-Handys

Unter **Leistung → Hintergrundmodus** kannst du optional 15, 30 oder 60 FPS als Android-Anzeigevorgabe wählen und die Windows-Audiositzung dieses Handys stummschalten, während eine andere App oder ein anderes Handy aktiv ist. Beim Zurückkehren oder Schließen des Kontrollfensters werden normale FPS und vorheriger Stummzustand wiederhergestellt. Andere Anwendungen und die Android-Lautstärke bleiben unabhängig. Spiele können anders rendern; dies ist eine Anzeigevorgabe. Die Optionen gelten bei geöffnetem Kontrollfenster dieses Handys.

### Sicherungen und Startreparatur

Unter **Wiederherstellung → Zeitplan & Reparatur** findest du **Velox Care**. Tägliche oder wöchentliche Sicherungen laufen ausschließlich bei ausgeschaltetem Handy und geöffnetem Kontrollfenster. Aktive Spiele werden nie automatisch beendet. Anzahl und logisches GB-Limit gelten für automatische Sicherungen dieses Handys; manuelle Sicherungen zählen separat.

Bei einem Limit oder Fehler pausiert der Zeitplan. Verschiebe ältere automatische Sicherungen aus dem Tresor, behebe die Ursache oder erhöhe ein Limit und wähle **Zeitplan fortsetzen**. Wiederherstellungspunkte werden nie automatisch gelöscht. Sicherungen und vollständige Klone enthalten private Daten und sind **nicht verschlüsselt**.

**Start abbrechen** beendet nur den Emulator, der zum Start dieses Handys gehört. Die **Startreparatur** prüft Googles Werkzeuge, erstellt die Gerätekonfiguration neu und umgeht veraltete Schnellstart-Zustände. Apps, Konten, Spielstände und gewählte Leistungseinstellungen bleiben erhalten. Fehlende Datenträger benötigen eine geprüfte Tresorsicherung; die Reparatur löscht oder ersetzt sie nicht.
