package com.cyanogenmod.defyparts;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

public class BpPanicHandlerService extends Service {
    private static final String TAG = "BpPanicHandlerService";
    private static final int NOTIFICATION_ID = 1;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.d(TAG, "Got BP panic");

        if (intent.getBooleanExtra("reboot", false)) {
            doReboot();
            return START_NOT_STICKY;
        }

        startForeground(NOTIFICATION_ID, createNotification());
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private Notification createNotification() {
        Intent rebootIntent = new Intent(this, getClass());
        rebootIntent.putExtra("reboot", true);

        PendingIntent pi = PendingIntent.getService(this, 0, rebootIntent, 0);

        Notification notification = new Notification(android.R.drawable.stat_notify_error, null, System.currentTimeMillis());
        notification.flags |= Notification.DEFAULT_ALL | Notification.FLAG_ONGOING_EVENT;
        notification.setLatestEventInfo(this, getString(R.string.modem_error), getString(R.string.touch_to_reboot), pi);

        return notification;
    }

    private void doReboot() {
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        pm.reboot("bppanic");
    }
}
