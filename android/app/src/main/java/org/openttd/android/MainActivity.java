package org.openttd.android;

import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import androidx.appcompat.app.AppCompatActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "OpenTTD";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
    }

    @Override
    protected void onStart() {
        super.onStart();
        startGame();
    }

    private void startGame() {
        copyAssets();

        Intent intent = new Intent(this, GameActivity.class);

        // Set the data path so OpenTTD can find baseset files.
        String dataPath = getFilesDir().getAbsolutePath();
        intent.putExtra("commandLineArgs", new String[]{
                "-s", "null",   // null sound driver
                "-m", "null",   // null music driver
                "-d", "9"       // verbose debug logging (all subsystems)
        });

        // Pass the data path to native code via environment variable.
        // GameActivity (SDLActivity) will pick this up before calling SDL_main.
        try {
            android.system.Os.setenv("OPENTTD_DATA_PATH", dataPath, true);
        } catch (android.system.ErrnoException e) {
            Log.e(TAG, "Failed to set OPENTTD_DATA_PATH", e);
        }

        startActivity(intent);
        finish();
    }

    private void copyAssets() {
        copyAssetsStatic(this);
    }

    /**
     * Copy baseset and lang assets from APK to internal storage so OpenTTD can read them.
     * Assets are expected at: assets/baseset/ and assets/lang/ in the APK.
     * They are copied to:     context.getFilesDir()/baseset/ and context.getFilesDir()/lang/
     *
     * This is static so it can be called from both the Activity and the WallpaperService.
     */
    static void copyAssetsStatic(Context context) {
        File dataDir = context.getFilesDir();
        AssetManager assets = context.getAssets();
        Log.i(TAG, "Data dir: " + dataDir.getAbsolutePath());

        try {
            String[] assetList = assets.list("baseset");
            Log.i(TAG, "Assets in baseset/: " + (assetList != null ? assetList.length : "null"));
            if (assetList != null) {
                for (String f : assetList) Log.i(TAG, "  asset: " + f);
            }
            copyAssetDir(assets, "baseset", new File(dataDir, "baseset"));
        } catch (IOException e) {
            Log.e(TAG, "Error extracting baseset assets", e);
        }

        try {
            copyAssetDir(assets, "lang", new File(dataDir, "lang"));
        } catch (IOException e) {
            Log.e(TAG, "Error extracting lang assets", e);
        }

        // Bundled title maps are provisioned exactly once (tracked by a persistent pref),
        // not re-copied on every restart, so a user-deleted bundled map stays deleted.
        // Tradeoff: a title map added by a future app update won't auto-copy once provisioned.
        if (!SettingsHelper.isTitleAssetsProvisioned(context)) {
            try {
                copyAssetDir(assets, "title", new File(dataDir, "title"));
                SettingsHelper.setTitleAssetsProvisioned(context);
            } catch (IOException e) {
                Log.e(TAG, "Error extracting title assets", e);
            }
        }

        // Log what ended up on disk
        File basesetDir = new File(dataDir, "baseset");
        if (basesetDir.exists()) {
            String[] files = basesetDir.list();
            Log.i(TAG, "Files in " + basesetDir + ": " + (files != null ? files.length : "null"));
            if (files != null) for (String f : files) Log.i(TAG, "  file: " + f);
        } else {
            Log.e(TAG, "baseset dir does not exist: " + basesetDir);
        }
    }

    private static void copyAssetDir(AssetManager assets, String srcPath, File destDir) throws IOException {
        String[] list = assets.list(srcPath);
        if (list == null) return;

        if (list.length == 0) {
            // It's a file — copy it.
            if (!destDir.getParentFile().exists()) {
                destDir.getParentFile().mkdirs();
            }
            try (InputStream in = assets.open(srcPath);
                 OutputStream out = new FileOutputStream(destDir)) {
                byte[] buf = new byte[8192];
                int len;
                while ((len = in.read(buf)) > 0) {
                    out.write(buf, 0, len);
                }
            }
            return;
        }

        // It's a directory — recurse.
        if (!destDir.exists()) {
            destDir.mkdirs();
        }
        for (String fileName : list) {
            copyAssetDir(assets, srcPath + "/" + fileName, new File(destDir, fileName));
        }
    }
}
