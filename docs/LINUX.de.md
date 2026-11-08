# Velox unter Linux

[English](LINUX.md)

Das AppImage enthält die Velox-Oberfläche, die benötigte Python-/Qt-Laufzeit,
Steuerungen und originale Hintergrundbilder. Android wird nach Annahme der
SDK-Lizenz direkt von Google heruntergeladen. Eine separate Installation des
Android-SDKs oder von Python ist nicht erforderlich.

## Voraussetzungen

- Ein x86-64-Desktop mit glibc ab 2.34 und der C++-Laufzeit von GCC 11 oder neuer.
- Ein funktionierender X11- oder Wayland-Desktop mit Grafiktreiber, Schriften und OpenGL.
- CPU-Virtualisierung und die Berechtigung, `/dev/kvm` mit deinem Benutzerkonto zu öffnen.
- Mindestens 6 GB RAM, empfohlen sind mindestens 8 GB. Halte mindestens 10 GB frei.
- FUSE mit `fusermount3` oder `fusermount` zum normalen Einhängen des AppImages.

Das Paket richtet sich an aktuelle glibc-Distributionen wie Debian, Ubuntu, Mint,
Fedora, Arch, openSUSE und kompatible Enterprise-Desktops. Es ist kein ARM-,
Alpine-/musl- oder natives NixOS-Paket. Unterschiede bei Distributionen und
Treibern können die Kompatibilität weiterhin beeinflussen.

Die verpackte Oberfläche wurde unter Debian 12 und AlmaLinux 9 auf Englisch und
Deutsch geprüft. Android-Einrichtung, KVM-Starts, Wiederherstellung, Klonen und
Steuerungen wurden unter Debian 12 getestet. Die native Mausaufnahme wurde auf
isolierten X11- und Wayland-Desktops geprüft. Diese Prüfungen decken nicht jede
Distribution, physische Grafikkarte oder jedes Controller-Modell ab.

## Erster Start

Lade AppImage und `SHA256SUMS.txt` aus derselben GitHub-Veröffentlichung herunter.
Prüfe die Datei, bevor du sie ausführbar machst:

```sh
sha256sum --check --ignore-missing SHA256SUMS.txt
chmod +x Velox-linux-x86_64.AppImage
./Velox-linux-x86_64.AppImage
```

Starte Velox mit deinem normalen Benutzerkonto. Beachte die Prüfungen für
Virtualisierung, KVM und Speicherplatz, lies Googles Lizenz und lasse die
Einrichtung abschließen. Downloads sind auf geprüfte Versionen festgelegt und
werden verifiziert. Unterbrochene Android-Downloads lassen sich fortsetzen.
Velox-Veröffentlichungen aktualisieren die geprüften SDK-Versionen zusammen mit
der Anwendung.

Die KVM-Berechtigungen verwaltet deine Distribution. Bei Desktops mit einer
`kvm`-Gruppe musst du dich nach dem Hinzufügen deines Kontos normalerweise ab- und
wieder anmelden. Verwende die dokumentierte Vorgehensweise deiner Distribution.
Velox startet den Desktop nicht als Root und gibt `/dev/kvm` nicht für alle frei.

Falls FUSE fehlt, kannst du das Paket in einen eigenen Ordner entpacken:

```sh
mkdir -p "$HOME/Applications/Velox"
cd "$HOME/Applications/Velox"
/pfad/zum/Velox-linux-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun
```

Die entpackte Version führt dieselbe Anwendung aus. Direkte AppImage-Updates und
der Menüeintrag erfordern den Start des AppImages selbst. Halte entpackte
Programmdateien vom Android-Datenordner getrennt.

## Daten und Updates

Daten liegen standardmäßig in `$XDG_DATA_HOME/velox` beziehungsweise
`~/.local/share/velox`. Mit `--data-dir /pfad/zum/ordner` wählst du einen anderen
privaten Datenordner. AppImages enthalten keine Konten, Handydaten oder SDK-Abbilder.
Wenn du die Oberfläche schließt oder die Handyansicht ausblendest, bleiben laufende
Handys für die nächste Velox-Sitzung verfügbar.

Die Updates-Seite prüft den signierten Linux-Updatefeed und die Prüfsumme des
AppImages vor dem Ersetzen. Updates sind eine bewusste Auswahl. Das vorherige
AppImage bleibt neben dem Programm in einem privaten `.velox-update-*`-Ordner
erhalten. Nach einem erfolgreichen Neustart kannst du diese vorherige Version
löschen, sobald du sie nicht mehr benötigst. Handys und Sicherungen bleiben im
Datenordner.

Über **Zum Anwendungsmenü hinzufügen** erstellst du optional einen Menüeintrag für
dein Benutzerkonto. Lass das AppImage anschließend an seinem Speicherort. Nach
einem Verschieben erstellst du den Eintrag vom neuen Speicherort aus erneut.

## Selbst bauen

Siehe die [Linux-Bauanleitung](LINUX-BUILD.md). Die Veröffentlichung bietet außerdem
ein Quelltextarchiv für Qt/PySide, die enthaltenen Systembibliotheken und die AppImage-Laufzeit. Lizenzen und
Versionsangaben findest du im entpackten AppImage unter
`usr/share/licenses/velox` und `usr/share/velox/build-info.json`.
