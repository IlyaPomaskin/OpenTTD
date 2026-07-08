package org.openttd.android;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageButton;
import android.widget.ListView;
import android.widget.TextView;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.MaterialToolbar;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class MapsActivity extends AppCompatActivity {

    private final List<File> mapFiles = new ArrayList<>();
    private MapAdapter adapter;
    private ListView listView;
    private TextView emptyView;

    private final ActivityResultLauncher<String> filePicker =
        registerForActivityResult(new ActivityResultContracts.GetContent(), this::onFilePicked);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        setContentView(R.layout.activity_maps);

        MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        toolbar.setNavigationOnClickListener(v -> finish());

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.app_bar), (v, insets) -> {
            int top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top;
            v.setPadding(v.getPaddingLeft(), top, v.getPaddingRight(), v.getPaddingBottom());
            return insets;
        });
        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.btn_add_map), (v, insets) -> {
            int bottom = insets.getInsets(WindowInsetsCompat.Type.systemBars()).bottom;
            ViewGroup.MarginLayoutParams lp = (ViewGroup.MarginLayoutParams) v.getLayoutParams();
            lp.bottomMargin = 16 + bottom;
            v.setLayoutParams(lp);
            return insets;
        });

        listView = findViewById(R.id.list_maps);
        emptyView = findViewById(R.id.txt_maps_empty);

        adapter = new MapAdapter();
        listView.setAdapter(adapter);

        findViewById(R.id.btn_add_map).setOnClickListener(v -> filePicker.launch("*/*"));

        refreshList();
    }

    private void onFilePicked(Uri uri) {
        if (uri == null) return;
        try {
            String fileName = "map.sav";
            try (android.database.Cursor cursor = getContentResolver().query(
                    uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int idx = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                    if (idx >= 0) fileName = cursor.getString(idx);
                }
            }
            if (!fileName.toLowerCase().endsWith(".sav")) fileName = fileName + ".sav";
            File titleDir = getTitleDir();
            titleDir.mkdirs();
            File dest = new File(titleDir, fileName);
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dest)) {
                if (in == null) return;
                byte[] buf = new byte[8192];
                int len;
                while ((len = in.read(buf)) > 0) out.write(buf, 0, len);
            }
            refreshList();
            sendTitleMapsChanged();
        } catch (Exception e) {
            // silently ignore
        }
    }

    private void confirmDelete(File file) {
        new AlertDialog.Builder(this)
            .setMessage(getString(R.string.maps_delete_confirm, file.getName()))
            .setPositiveButton(R.string.maps_delete, (dialog, which) -> {
                if (file.delete()) {
                    refreshList();
                    sendTitleMapsChanged();
                }
            })
            .setNegativeButton(R.string.cancel, null)
            .show();
    }

    private void refreshList() {
        mapFiles.clear();
        File titleDir = getTitleDir();
        if (titleDir.exists()) {
            File[] files = titleDir.listFiles(f ->
                f.isFile() && f.getName().toLowerCase().endsWith(".sav"));
            if (files != null) {
                Arrays.sort(files, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
                mapFiles.addAll(Arrays.asList(files));
            }
        }
        adapter.notifyDataSetChanged();
        boolean empty = mapFiles.isEmpty();
        listView.setVisibility(empty ? View.GONE : View.VISIBLE);
        emptyView.setVisibility(empty ? View.VISIBLE : View.GONE);
    }

    private File getTitleDir() {
        return new File(getFilesDir(), "title");
    }

    private void sendTitleMapsChanged() {
        sendBroadcast(new Intent(SettingsHelper.ACTION_TITLE_MAPS_CHANGED));
    }

    private class MapAdapter extends BaseAdapter {
        @Override public int getCount() { return mapFiles.size(); }
        @Override public File getItem(int position) { return mapFiles.get(position); }
        @Override public long getItemId(int position) { return position; }
        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(MapsActivity.this)
                    .inflate(R.layout.item_map_file, parent, false);
            }
            File file = getItem(position);
            ((TextView) convertView.findViewById(R.id.txt_map_name)).setText(file.getName());
            ImageButton btnDelete = convertView.findViewById(R.id.btn_delete_map);
            boolean canDelete = mapFiles.size() > 1;
            btnDelete.setEnabled(canDelete);
            btnDelete.setAlpha(canDelete ? 1.0f : 0.3f);
            btnDelete.setOnClickListener(v -> confirmDelete(file));
            return convertView;
        }
    }
}
