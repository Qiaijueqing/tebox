import android.content.ComponentName;
import android.os.IBinder;
import java.lang.reflect.Method;

/* First-boot helper (run by init as root through app_process): switch the
 * ColorOS default video live wallpaper, which needs a hardware video decode
 * path, to SystemUI's static ImageWallpaper. Acts as uid 1000 so the
 * wallpaper service accepts the "android" calling package.
 *
 * ColorOS applies its own default live wallpaper once more shortly after the
 * first boot completes, so keep watching for a minute and switch back if it
 * does. Later boots keep whatever is set. */
public class SetWallpaper {
    private static final String[] VIDEO_WALLPAPERS = {
        "com.heytap.colorfulengine", "com.android.wallpaper.livepicker",
    };

    public static void main(String[] args) throws Exception {
        android.system.Os.setgid(1000);
        android.system.Os.setuid(1000);
        IBinder b = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, "wallpaper");
        Object wm = Class.forName("android.app.IWallpaperManager$Stub")
                .getMethod("asInterface", IBinder.class).invoke(null, b);
        ComponentName image = new ComponentName("com.android.systemui",
                "com.android.systemui.wallpapers.ImageWallpaper");
        Class<?> builder = Class.forName("android.app.wallpaper.WallpaperDescription$Builder");
        Object desc = builder.getConstructor().newInstance();
        builder.getMethod("setComponent", ComponentName.class).invoke(desc, image);
        desc = builder.getMethod("build").invoke(desc);
        Method set = wm.getClass().getMethod("setWallpaperComponentChecked",
                desc.getClass(), String.class, int.class, int.class);
        Method info = wm.getClass().getMethod("getWallpaperInfo", int.class);

        set.invoke(wm, desc, "android", 1 | 2 /* FLAG_SYSTEM | FLAG_LOCK */, 0);
        System.out.println("wallpaper: " + image.flattenToShortString());
        for (int i = 0; i < 12; i++) {
            Thread.sleep(5000);
            Object current = info.invoke(wm, 0);  // null for ImageWallpaper
            String pkg = current == null ? null
                    : ((android.app.WallpaperInfo) current).getPackageName();
            for (String video : VIDEO_WALLPAPERS) {
                if (video.equals(pkg)) {
                    set.invoke(wm, desc, "android", 1 | 2, 0);
                    System.out.println("wallpaper: replaced " + pkg + " again");
                }
            }
        }
    }
}
