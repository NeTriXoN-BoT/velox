package app.velox.home;
import android.content.Context;
import java.util.HashMap;
import java.util.Locale;
final class Strings {
    private static final HashMap<String,String> DE = new HashMap<>();
    static {
        DE.put("Accounts", "Konten");
        DE.put("Add to favorites", "Zu Favoriten hinzufügen");
        DE.put("All Android settings", "Alle Android-Einstellungen");
        DE.put("All apps", "Alle Apps");
        DE.put("Android navigation", "Android-Navigation");
        DE.put("Android settings", "Android-Einstellungen");
        DE.put("App information", "App-Informationen");
        DE.put("Appearance and layout for Velox Home.", "Aussehen und Anordnung von Velox Home.");
        DE.put("Apps and permissions", "Apps und Berechtigungen");
        DE.put("Automatically follow game orientation", "Der Spielausrichtung automatisch folgen");
        DE.put("Back to Velox Home", "Zurück zu Velox Home");
        DE.put("Back · Home · Recents", "Zurück · Home · Übersicht");
        DE.put("Choose English or German for Velox. Android's system language stays as it is.", "Wähle Englisch oder Deutsch für Velox. Die Android-Systemsprache bleibt unverändert.");
        DE.put("Choose a wallpaper from Look in desktop Velox.", "Wähle ein Hintergrundbild unter „Design“ in Velox auf dem PC.");
        DE.put("Choose default Home app", "Standard-Startbildschirm wählen");
        DE.put("Choose wallpaper", "Hintergrundbild auswählen");
        DE.put("Connected to your desktop", "Mit deinem PC verbunden");
        DE.put("Default home", "Standard-Startbildschirm");
        DE.put("Desktop Velox is connected. Navigation and orientation changes sync automatically.", "Velox auf dem PC ist verbunden. Änderungen an Navigation, Ausrichtung und Sprache werden automatisch synchronisiert.");
        DE.put("Deutsch", "Deutsch");
        DE.put("Display and text size", "Anzeige und Schriftgröße");
        DE.put("English", "English");
        DE.put("Favorites", "Favoriten");
        DE.put("Game orientation", "Spielausrichtung");
        DE.put("Games", "Spiele");
        DE.put("Games will appear here when Android identifies them as games. All apps always shows everything.", "Hier erscheinen Apps, die Android als Spiele erkennt. „Alle Apps“ zeigt sämtliche Apps.");
        DE.put("Gesture navigation", "Gestennavigation");
        DE.put("Hold an app to add it to your favorites.", "Halte eine App gedrückt, um sie zu deinen Favoriten hinzuzufügen.");
        DE.put("Keep Velox Home as your everyday launcher, or choose another home app.", "Verwende Velox Home als Startbildschirm oder wähle eine andere Home-App.");
        DE.put("Larger app tiles", "Größere App-Kacheln");
        DE.put("MAKE THIS PHONE YOURS", "DEIN HANDY. DEIN STIL.");
        DE.put("Make Velox my Home", "Velox als Startbildschirm nutzen");
        DE.put("Mark as game", "Als Spiel markieren");
        DE.put("Network and internet", "Netzwerk und Internet");
        DE.put("No matching apps.", "Keine passenden Apps.");
        DE.put("Open desktop Velox to apply navigation and orientation changes. Home appearance works independently.", "Öffne Velox auf dem PC, um Navigation, Ausrichtung und Sprache zu synchronisieren. Das Home-Design funktioniert unabhängig.");
        DE.put("Open the phone's original settings for system and account options.", "Öffne die ursprünglichen Einstellungen für System- und Kontooptionen.");
        DE.put("Recent", "Zuletzt");
        DE.put("Recents · Home · Back", "Übersicht · Home · Zurück");
        DE.put("Remove from Games", "Aus Spielen entfernen");
        DE.put("Remove from favorites", "Aus Favoriten entfernen");
        DE.put("Reversed buttons need Root and an initial phone restart. Your apps keep their normal direction.", "Die umgekehrte Tastenreihenfolge benötigt Root und einen einmaligen Handyneustart. Apps behalten ihre normale Ausrichtung.");
        DE.put("Search your games and apps", "Deine Spiele und Apps suchen");
        DE.put("Search apps", "Apps suchen");
        DE.put("Settings", "Einstellungen");
        DE.put("Show clock and date", "Uhr und Datum anzeigen");
        DE.put("Sound and volume", "Ton und Lautstärke");
        DE.put("These controls sync with desktop Velox while it is connected.", "Diese Einstellungen werden mit Velox auf dem PC synchronisiert, solange eine Verbindung besteht.");
        DE.put("This app couldn't be opened.", "Diese App konnte nicht geöffnet werden.");
        DE.put("This setting isn't available on this phone.", "Diese Einstellung ist auf diesem Handy nicht verfügbar.");
        DE.put("VELOX  /  YOUR GAME. YOUR SPACE.", "VELOX  /  DEIN SPIEL. DEIN PLATZ.");
        DE.put("Velox Settings", "Velox-Einstellungen");
        DE.put("Velox language", "Velox-Sprache");
        DE.put("Your play space", "Dein Platz zum Spielen");
        DE.put("Your recently opened apps will appear here.", "Hier erscheinen deine zuletzt geöffneten Apps.");
    }
    static String text(Context context, String value) {
        if (Catalog.prefs(context).getInt("language",0) != 1) return value;
        String translated=DE.get(value);
        return translated == null ? value : translated;
    }
    static String[] texts(Context context,String[] values) {
        String[] result=values.clone();
        for(int n=0;n<result.length;n++) result[n]=text(context,result[n]);
        return result;
    }
    static Locale locale(Context context) { return Catalog.prefs(context).getInt("language",0)==1 ? Locale.GERMAN : Locale.ENGLISH; }
}
