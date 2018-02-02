package com.inorris.witness.androidclient;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.icu.text.SimpleDateFormat;
import android.icu.util.Calendar;
import android.media.AudioAttributes;
import android.net.Uri;
import android.os.IBinder;
import android.provider.MediaStore;
import android.support.v7.app.NotificationCompat;
import android.util.Log;

import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

import java.io.IOException;
import java.net.URL;
import java.util.Map;

public class FirebaseServiceMessage extends FirebaseMessagingService {

    private static final int TEMPORARY_NOTIFICATION_ID = 123;

    public FirebaseServiceMessage() {
    }

    @Override
    public void onMessageReceived(RemoteMessage remoteMessage) {

        try {
            sendNotification( remoteMessage );
        } catch (Exception e) {
            e.printStackTrace();
        }

        // Also if you intend on generating your own notifications as a result of a received FCM
        // message, here is where that should be initiated. See sendNotification method below.

        super.onMessageReceived( remoteMessage );
    }

    private void sendNotification(final RemoteMessage remoteMessage) throws Exception {

        Map<String,String> Data = remoteMessage.getData();
        String Alert = Data.get("alert");
        String Image = Data.get("image");
        String Camera = Data.get("cameraSource");

        String contentTitle = "Motion at " + Camera;

        sendNotification(getApplicationContext(),
                android.R.drawable.ic_menu_view,
                contentTitle,
                Alert,
                0,
                MainActivity.class, Image );

    }

    public void sendNotification(Context appContext,
                                       int icon,
                                       String title,
                                       String msg,
                                       long when,
                                       Class<? extends Context> classToLaunch,
                                       String imageUri ) {

        //Define notification msg
        Intent launchIntent = null;

        if (classToLaunch != null) {
            launchIntent = new Intent(appContext, classToLaunch);
        } else {
            launchIntent = new Intent();
        }

        // This is dummy data for just differentiate Pending intent
        // only set value that is check IntentFilter
        launchIntent.addCategory("Camera");

        // also make launch mode to singleTop in manifest for that activity
        launchIntent.setFlags(
                Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TASK |
                        Intent.FLAG_ACTIVITY_NEW_TASK);

        // intent to be launched when click on notification
        PendingIntent pendingIntent = PendingIntent.getActivity(appContext,
                0,
                launchIntent,
                PendingIntent.FLAG_UPDATE_CURRENT);

        NotificationManager notificationManager = (NotificationManager) appContext.getSystemService(Context.NOTIFICATION_SERVICE);

        AudioAttributes.Builder att = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_NOTIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_UNKNOWN);

        NotificationChannel channel = new NotificationChannel( "WitnessChannel", "WitnessChannel", NotificationManager.IMPORTANCE_HIGH );
        channel.setDescription("Front Door");
        channel.enableLights(true);
        channel.setLightColor(Color.MAGENTA);
        channel.setVibrationPattern( new long[] { 0, 1000, 500, 1000});
        channel.setBypassDnd(true);
        channel.setShowBadge(true);
        channel.setSound( Uri.parse("android.resource://"+appContext.getPackageName()+"/"+R.raw.doorbell), att.build() );

        channel.enableVibration(true);
        notificationManager.createNotificationChannel(channel);

        //Instantiate the notification
        Notification.Builder builder = new Notification.Builder(appContext, "WitnessChannel");
        builder.setContentTitle(title);
        builder.setSmallIcon(icon);
        builder.setWhen(when);
        builder.setTicker(msg);
        builder.setContentText(msg);
        builder.setContentIntent(pendingIntent);
        builder.setAutoCancel(true);

        builder.addAction( new Notification.Action( icon, "Done", pendingIntent ) );
        builder.addAction( new Notification.Action( icon, "Live", pendingIntent ) );

        try {
            URL url = new URL( imageUri );
            Bitmap image = BitmapFactory.decodeStream( url.openConnection().getInputStream() );
            builder.setStyle( new Notification.BigPictureStyle().bigPicture( image ) );
        } catch (IOException e) {
            e.printStackTrace();
        }

        notificationManager.notify( TEMPORARY_NOTIFICATION_ID, builder.build());
    }
}
