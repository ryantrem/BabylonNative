package BabylonNative;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;

import android.opengl.GLES20;
import android.opengl.GLES11Ext;
import android.opengl.GLU;

import java.lang.ref.WeakReference;
import java.util.ArrayList;

public class BabylonView extends TextureView implements TextureView.SurfaceTextureListener, View.OnTouchListener {
    private static final String TAG = "BabylonView";
    private boolean mViewReady = false;
    private ViewDelegate mViewDelegate;

    public BabylonView(Context context, ViewDelegate viewDelegate) {
        super(context);
        init(viewDelegate);
        BabylonNative.Wrapper.initEngine(context.getResources().getAssets());

        /*Handler handler = new Handler(Looper.getMainLooper());
        handler.post(new Runnable()
        {
            public void run() {
                int[] textures = new int[1];
                GLES20.glGenTextures(1, textures, 0);
                int error = GLES20.glGetError();
                Log.d("mytag", "TEXTURE ERROR " + GLU.gluErrorString(error));
                Log.d("mytag", "TEXTURE ID " + textures[0]);

                GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
                GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
                GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
                GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
                GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);

                SurfaceTexture surfaceTexture = new SurfaceTexture(textures[0], false);
                setSurfaceTexture(surfaceTexture);

                BabylonNative.Wrapper.surfaceCreated(new Surface(surfaceTexture));
                if (!mViewReady) {
                    mViewDelegate.onViewReady();
                    mViewReady = true;
                }
            }
        });*/

        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        int error = GLES20.glGetError();
        Log.d("mytag", "TEXTURE ERROR " + GLU.gluErrorString(error));
        Log.d("mytag", "TEXTURE ID " + textures[0]);

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);

        SurfaceTexture surfaceTexture = new SurfaceTexture(textures[0], false);
        setSurfaceTexture(surfaceTexture);

        BabylonNative.Wrapper.surfaceCreated(new Surface(surfaceTexture));
        /*if (!mViewReady) {
            mViewDelegate.onViewReady();
            mViewReady = true;
        }*/
    }

    private void init(ViewDelegate viewDelegate) {
        setSurfaceTextureListener(this);
        setOnTouchListener(this);
        mViewDelegate = viewDelegate;
    }

    public void loadScript(String path) {
        BabylonNative.Wrapper.loadScript(path);
    }

    public void eval(String source, String sourceURL) {
        BabylonNative.Wrapper.eval(source, sourceURL);
    }

    public void onPause() {
        setVisibility(View.GONE);
        BabylonNative.Wrapper.activityOnPause();
    }

    public void onResume() {
        BabylonNative.Wrapper.activityOnResume();
    }

    /**
     * This method is part of the SurfaceHolder.Callback interface, and is
     * not normally called or subclassed by clients of BabylonView.
     */
    public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
       /* int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        int error = GLES20.glGetError();
        Log.d("mytag", "TEXTURE ERROR " + GLU.gluErrorString(error));
        Log.d("mytag", "TEXTURE ID " + textures[0]);

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);

        surface = new SurfaceTexture(textures[0], false);
        //surfaceTexture.detachFromGLContext();
        setSurfaceTexture(surface);*/

        BabylonNative.Wrapper.surfaceCreated(new Surface(surface));
        if (!this.mViewReady) {
            mViewDelegate.onViewReady();
            mViewReady = true;
        }
    }

    /**
     * This method is part of the SurfaceHolder.Callback interface, and is
     * not normally called or subclassed by clients of BabylonView.
     */
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
        return true;
    }

    /**
     * This method is part of the SurfaceHolder.Callback interface, and is
     * not normally called or subclassed by clients of BabylonView.
     */
    public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int w, int h) {
        BabylonNative.Wrapper.surfaceChanged(w, h, new Surface(surface));
    }

    public void onSurfaceTextureUpdated(SurfaceTexture surface) {
        BabylonNative.Wrapper.surfaceChanged(getWidth(), getHeight(), new Surface(surface));
    }

    public interface ViewDelegate {
        void onViewReady();
    }

    @Override
    public boolean onTouch(View v, MotionEvent event)
    {
        float mX = event.getX();
        float mY = event.getY();
        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                BabylonNative.Wrapper.setTouchInfo(mX, mY, true);
                break;
            case MotionEvent.ACTION_MOVE:
                BabylonNative.Wrapper.setTouchInfo(mX, mY, true);
                break;
            case MotionEvent.ACTION_UP:
                BabylonNative.Wrapper.setTouchInfo(mX, mY, false);
                break;
        }
        return true;
    }

    @Override
    protected void finalize() throws Throwable {
        BabylonNative.Wrapper.finishEngine();
    }
}
