package org.openttd.android;

import android.os.Bundle;

import androidx.appcompat.app.AppCompatActivity;

public class AssetSetupActivity extends AppCompatActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_asset_setup);

        if (getSupportActionBar() != null) {
            getSupportActionBar().setTitle(R.string.asset_setup_title);
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
        }
    }

    @Override
    public boolean onSupportNavigateUp() {
        finish();
        return true;
    }
}
