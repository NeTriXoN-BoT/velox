package app.velox.home;
import android.app.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.*;
import android.view.*;
public class UiTests extends Instrumentation {
    void checkHeader(View view){
        if(view.getClass().getName().endsWith("Design$Mark")){android.graphics.Rect visible=new android.graphics.Rect();if(!view.getGlobalVisibleRect(visible)||visible.height()<view.getHeight())throw new AssertionError("Home header was clipped after rotation");}
        if(view instanceof ViewGroup)for(int i=0;i<((ViewGroup)view).getChildCount();i++)checkHeader(((ViewGroup)view).getChildAt(i));
    }
    boolean hasText(View v,String text){if(v instanceof android.widget.TextView && text.contentEquals(((android.widget.TextView)v).getText()))return true;if(v instanceof ViewGroup)for(int i=0;i<((ViewGroup)v).getChildCount();i++)if(hasText(((ViewGroup)v).getChildAt(i),text))return true;return false;}
    boolean clickText(View v,String text){if(v instanceof android.widget.TextView && text.contentEquals(((android.widget.TextView)v).getText()))return v.performClick();if(v instanceof ViewGroup)for(int i=0;i<((ViewGroup)v).getChildCount();i++)if(clickText(((ViewGroup)v).getChildAt(i),text))return true;return false;}
    public void onCreate(Bundle args){super.onCreate(args);start();}
    public void onStart(){Activity activity=null;try{
        activity=startActivitySync(new Intent().setClassName("app.velox.home","app.velox.home.HomeActivity").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        final Activity home=activity;
        java.lang.reflect.Field cached=home.getClass().getDeclaredField("cachedApps");cached.setAccessible(true);
        Object originalCache=cached.get(home);
        java.lang.reflect.Field searchField=home.getClass().getDeclaredField("search");searchField.setAccessible(true);
        android.widget.EditText search=(android.widget.EditText)searchField.get(home);
        runOnMainSync(()->search.setText("velox-no-match-test"));waitForIdleSync();
        if(cached.get(home)!=originalCache)throw new AssertionError("Search queried the app catalog again");
        runOnMainSync(()->search.setText(""));waitForIdleSync();
        java.util.List<?> apps=(java.util.List<?>)originalCache;
        if(apps.isEmpty())throw new AssertionError("No real apps in Home");
        Object first=apps.get(0);java.lang.reflect.Field componentField=first.getClass().getDeclaredField("component"),favoriteField=first.getClass().getDeclaredField("favorite");componentField.setAccessible(true);favoriteField.setAccessible(true);
        String key="fav:"+componentField.get(first);android.content.SharedPreferences prefs=home.getSharedPreferences("velox",0);boolean existed=prefs.contains(key),originalFavorite=prefs.getBoolean(key,false);
        try{
            runOnMainSync(()->prefs.edit().putBoolean(key,!originalFavorite).apply());Thread.sleep(350);waitForIdleSync();
            java.util.List<?> refreshed=(java.util.List<?>)cached.get(home);Object updated=refreshed.get(0);
            if(favoriteField.getBoolean(updated)==originalFavorite)throw new AssertionError("Home did not refresh a changed favorite");
        }finally{
            runOnMainSync(()->{android.content.SharedPreferences.Editor edit=prefs.edit();if(existed)edit.putBoolean(key,originalFavorite);else edit.remove(key);edit.apply();});Thread.sleep(200);waitForIdleSync();
        }

        runOnMainSync(()->home.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE));waitForIdleSync();Thread.sleep(800);
        if(home.getResources().getConfiguration().orientation!=Configuration.ORIENTATION_LANDSCAPE)throw new AssertionError("Home did not enter landscape");
        runOnMainSync(()->checkHeader(home.getWindow().getDecorView()));
        Bundle phase=new Bundle();phase.putString("phase","LANDSCAPE");sendStatus(0,phase);Thread.sleep(1800);
        runOnMainSync(()->home.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT));waitForIdleSync();Thread.sleep(600);
        if(home.getResources().getConfiguration().orientation!=Configuration.ORIENTATION_PORTRAIT)throw new AssertionError("Home did not return to portrait");
        runOnMainSync(()->checkHeader(home.getWindow().getDecorView()));
        phase=new Bundle();phase.putString("phase","PORTRAIT");sendStatus(0,phase);Thread.sleep(1800);
        runOnMainSync(()->home.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED));
        String systemLocale=home.getResources().getConfiguration().getLocales().toLanguageTags();
        final Activity settings=startActivitySync(new Intent().setClassName("app.velox.home","app.velox.home.SettingsActivity").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        waitForIdleSync();
        runOnMainSync(()->{if(!clickText(settings.getWindow().getDecorView(),"English"))throw new AssertionError("English action missing");});Thread.sleep(200);waitForIdleSync();
        runOnMainSync(()->{if(!hasText(settings.getWindow().getDecorView(),"Velox Settings")||!hasText(settings.getWindow().getDecorView(),"Automatically follow game orientation"))throw new AssertionError("English settings translation");if(!clickText(settings.getWindow().getDecorView(),"Deutsch"))throw new AssertionError("German action missing");});Thread.sleep(200);waitForIdleSync();
        runOnMainSync(()->{if(!hasText(settings.getWindow().getDecorView(),"Velox-Einstellungen")||!hasText(settings.getWindow().getDecorView(),"Der Spielausrichtung automatisch folgen"))throw new AssertionError("German settings translation");});
        if(!systemLocale.equals(home.getResources().getConfiguration().getLocales().toLanguageTags()))throw new AssertionError("Velox changed Android system locale");
        runOnMainSync(()->settings.finish());Thread.sleep(250);waitForIdleSync();
        runOnMainSync(()->{if(!hasText(home.getWindow().getDecorView(),"Einstellungen"))throw new AssertionError("Home did not refresh its language");});

        Bundle result=new Bundle();result.putString("result","PASS: cached search, live favorites, unclipped portrait/landscape Home, English/German Settings and live Home language without changing Android locale/rotation");finish(Activity.RESULT_OK,result);
    }catch(Throwable error){Bundle result=new Bundle();result.putString("error",error.toString());finish(Activity.RESULT_CANCELED,result);}}
}
