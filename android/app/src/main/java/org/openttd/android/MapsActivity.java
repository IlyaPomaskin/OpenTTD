package org.openttd.android;

import android.app.AlertDialog;
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
        setContentView(R.layout.activity_maps);

        if (getSupportActionBar() != null) {
            getSupportActionBar().setTitle(R.string.title_maps);
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
        }

        listView = findViewById(R.id.list_maps);
        emptyView = findViewById(R.id.txt_maps_empty);

        adapter = new MapAdapter();
        listView.setAdapter(adapter);

        findViewById(R.id.btn_add_map).setOnClickListener(v -> filePicker.launch("*/*"));

        refreshList();
    }

    @Override
    public boolean onSupportNavigateUp() {
        finish();
        return true;
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
        } catch (Exception e) {
            // silently ignore
        }
    }

    private void confirmDelete(File file) {
        new AlertDialog.Builder(this)
            .setMessage(getString(R.string.maps_delete_confirm, file.getName()))
            .setPositiveButton(R.string.maps_delete, (dialog, which) -> {
                file.delete();
                refreshList();
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
