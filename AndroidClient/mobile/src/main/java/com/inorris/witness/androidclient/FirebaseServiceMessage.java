package com.inorris.witness.androidclient;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.icu.text.SimpleDateFormat;
import android.icu.util.Calendar;
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
                MainActivity.class, 123, Image );

    }

    public void sendNotification(Context appContext,
                                       int icon,
                                       String title,
                                       String msg,
                                       long when,
                                       Class<? extends Context> classToLaunch,
                                       long processId,
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

        //Instantiate the notification
        NotificationCompat.Builder builder = new NotificationCompat.Builder(appContext); //(icon, msg, when);
        builder.setContentTitle(title);
        builder.setSmallIcon(icon);
        builder.setWhen(when);
        builder.setTicker(msg);
        builder.setContentText(msg);
        builder.setContentIntent(pendingIntent);
        builder.setAutoCancel(true);
        try {
            URL url = new URL( imageUri );
            Bitmap image = BitmapFactory.decodeStream( url.openConnection().getInputStream() );
            builder.setStyle( new NotificationCompat.BigPictureStyle().bigPicture( image ) );
        } catch (IOException e) {
            e.printStackTrace();
        }
        builder.setDefaults(Notification.DEFAULT_LIGHTS);
        builder.setDefaults(Notification.DEFAULT_SOUND);


        NotificationManager notificationManager = (NotificationManager) appContext.getSystemService(Context.NOTIFICATION_SERVICE);
        notificationManager.notify((int) processId, builder.build());
    }
}
