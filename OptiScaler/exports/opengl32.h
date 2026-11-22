#pragma once

#include "SysUtils.h"

#include <proxies/KernelBase_Proxy.h>

struct opengl32_dll
{
    HMODULE dll;
    FARPROC GlmfBeginGlsBlock;
    FARPROC GlmfCloseMetaFile;
    FARPROC GlmfEndGlsBlock;
    FARPROC GlmfEndPlayback;
    FARPROC GlmfInitPlayback;
    FARPROC GlmfPlayGlsRecord;
    FARPROC glAccum;
    FARPROC glAlphaFunc;
    FARPROC glAreTexturesResident;
    FARPROC glArrayElement;
    FARPROC glBegin;
    FARPROC glBindTexture;
    FARPROC glBitmap;
    FARPROC glBlendFunc;
    FARPROC glCallList;
    FARPROC glCallLists;
    FARPROC glClear;
    FARPROC glClearAccum;
    FARPROC glClearColor;
    FARPROC glClearDepth;
    FARPROC glClearIndex;
    FARPROC glClearStencil;
    FARPROC glClipPlane;
    FARPROC glColor3b;
    FARPROC glColor3bv;
    FARPROC glColor3d;
    FARPROC glColor3dv;
    FARPROC glColor3f;
    FARPROC glColor3fv;
    FARPROC glColor3i;
    FARPROC glColor3iv;
    FARPROC glColor3s;
    FARPROC glColor3sv;
    FARPROC glColor3ub;
    FARPROC glColor3ubv;
    FARPROC glColor3ui;
    FARPROC glColor3uiv;
    FARPROC glColor3us;
    FARPROC glColor3usv;
    FARPROC glColor4b;
    FARPROC glColor4bv;
    FARPROC glColor4d;
    FARPROC glColor4dv;
    FARPROC glColor4f;
    FARPROC glColor4fv;
    FARPROC glColor4i;
    FARPROC glColor4iv;
    FARPROC glColor4s;
    FARPROC glColor4sv;
    FARPROC glColor4ub;
    FARPROC glColor4ubv;
    FARPROC glColor4ui;
    FARPROC glColor4uiv;
    FARPROC glColor4us;
    FARPROC glColor4usv;
    FARPROC glColorMask;
    FARPROC glColorMaterial;
    FARPROC glColorPointer;
    FARPROC glCopyPixels;
    FARPROC glCopyTexImage1D;
    FARPROC glCopyTexImage2D;
    FARPROC glCopyTexSubImage1D;
    FARPROC glCopyTexSubImage2D;
    FARPROC glCullFace;
    FARPROC glDebugEntry;
    FARPROC glDeleteLists;
    FARPROC glDeleteTextures;
    FARPROC glDepthFunc;
    FARPROC glDepthMask;
    FARPROC glDepthRange;
    FARPROC glDisable;
    FARPROC glDisableClientState;
    FARPROC glDrawArrays;
    FARPROC glDrawBuffer;
    FARPROC glDrawElements;
    FARPROC glDrawPixels;
    FARPROC glEdgeFlag;
    FARPROC glEdgeFlagPointer;
    FARPROC glEdgeFlagv;
    FARPROC glEnable;
    FARPROC glEnableClientState;
    FARPROC glEnd;
    FARPROC glEndList;
    FARPROC glEvalCoord1d;
    FARPROC glEvalCoord1dv;
    FARPROC glEvalCoord1f;
    FARPROC glEvalCoord1fv;
    FARPROC glEvalCoord2d;
    FARPROC glEvalCoord2dv;
    FARPROC glEvalCoord2f;
    FARPROC glEvalCoord2fv;
    FARPROC glEvalMesh1;
    FARPROC glEvalMesh2;
    FARPROC glEvalPoint1;
    FARPROC glEvalPoint2;
    FARPROC glFeedbackBuffer;
    FARPROC glFinish;
    FARPROC glFlush;
    FARPROC glFogf;
    FARPROC glFogfv;
    FARPROC glFogi;
    FARPROC glFogiv;
    FARPROC glFrontFace;
    FARPROC glFrustum;
    FARPROC glGenLists;
    FARPROC glGenTextures;
    FARPROC glGetBooleanv;
    FARPROC glGetClipPlane;
    FARPROC glGetDoublev;
    FARPROC glGetError;
    FARPROC glGetFloatv;
    FARPROC glGetIntegerv;
    FARPROC glGetLightfv;
    FARPROC glGetLightiv;
    FARPROC glGetMapdv;
    FARPROC glGetMapfv;
    FARPROC glGetMapiv;
    FARPROC glGetMaterialfv;
    FARPROC glGetMaterialiv;
    FARPROC glGetPixelMapfv;
    FARPROC glGetPixelMapuiv;
    FARPROC glGetPixelMapusv;
    FARPROC glGetPointerv;
    FARPROC glGetPolygonStipple;
    FARPROC glGetString;
    FARPROC glGetTexEnvfv;
    FARPROC glGetTexEnviv;
    FARPROC glGetTexGendv;
    FARPROC glGetTexGenfv;
    FARPROC glGetTexGeniv;
    FARPROC glGetTexImage;
    FARPROC glGetTexLevelParameterfv;
    FARPROC glGetTexLevelParameteriv;
    FARPROC glGetTexParameterfv;
    FARPROC glGetTexParameteriv;
    FARPROC glHint;
    FARPROC glIndexMask;
    FARPROC glIndexPointer;
    FARPROC glIndexd;
    FARPROC glIndexdv;
    FARPROC glIndexf;
    FARPROC glIndexfv;
    FARPROC glIndexi;
    FARPROC glIndexiv;
    FARPROC glIndexs;
    FARPROC glIndexsv;
    FARPROC glIndexub;
    FARPROC glIndexubv;
    FARPROC glInitNames;
    FARPROC glInterleavedArrays;
    FARPROC glIsEnabled;
    FARPROC glIsList;
    FARPROC glIsTexture;
    FARPROC glLightModelf;
    FARPROC glLightModelfv;
    FARPROC glLightModeli;
    FARPROC glLightModeliv;
    FARPROC glLightf;
    FARPROC glLightfv;
    FARPROC glLighti;
    FARPROC glLightiv;
    FARPROC glLineStipple;
    FARPROC glLineWidth;
    FARPROC glListBase;
    FARPROC glLoadIdentity;
    FARPROC glLoadMatrixd;
    FARPROC glLoadMatrixf;
    FARPROC glLoadName;
    FARPROC glLogicOp;
    FARPROC glMap1d;
    FARPROC glMap1f;
    FARPROC glMap2d;
    FARPROC glMap2f;
    FARPROC glMapGrid1d;
    FARPROC glMapGrid1f;
    FARPROC glMapGrid2d;
    FARPROC glMapGrid2f;
    FARPROC glMaterialf;
    FARPROC glMaterialfv;
    FARPROC glMateriali;
    FARPROC glMaterialiv;
    FARPROC glMatrixMode;
    FARPROC glMultMatrixd;
    FARPROC glMultMatrixf;
    FARPROC glNewList;
    FARPROC glNormal3b;
    FARPROC glNormal3bv;
    FARPROC glNormal3d;
    FARPROC glNormal3dv;
    FARPROC glNormal3f;
    FARPROC glNormal3fv;
    FARPROC glNormal3i;
    FARPROC glNormal3iv;
    FARPROC glNormal3s;
    FARPROC glNormal3sv;
    FARPROC glNormalPointer;
    FARPROC glOrtho;
    FARPROC glPassThrough;
    FARPROC glPixelMapfv;
    FARPROC glPixelMapuiv;
    FARPROC glPixelMapusv;
    FARPROC glPixelStoref;
    FARPROC glPixelStorei;
    FARPROC glPixelTransferf;
    FARPROC glPixelTransferi;
    FARPROC glPixelZoom;
    FARPROC glPointSize;
    FARPROC glPolygonMode;
    FARPROC glPolygonOffset;
    FARPROC glPolygonStipple;
    FARPROC glPopAttrib;
    FARPROC glPopClientAttrib;
    FARPROC glPopMatrix;
    FARPROC glPopName;
    FARPROC glPrioritizeTextures;
    FARPROC glPushAttrib;
    FARPROC glPushClientAttrib;
    FARPROC glPushMatrix;
    FARPROC glPushName;
    FARPROC glRasterPos2d;
    FARPROC glRasterPos2dv;
    FARPROC glRasterPos2f;
    FARPROC glRasterPos2fv;
    FARPROC glRasterPos2i;
    FARPROC glRasterPos2iv;
    FARPROC glRasterPos2s;
    FARPROC glRasterPos2sv;
    FARPROC glRasterPos3d;
    FARPROC glRasterPos3dv;
    FARPROC glRasterPos3f;
    FARPROC glRasterPos3fv;
    FARPROC glRasterPos3i;
    FARPROC glRasterPos3iv;
    FARPROC glRasterPos3s;
    FARPROC glRasterPos3sv;
    FARPROC glRasterPos4d;
    FARPROC glRasterPos4dv;
    FARPROC glRasterPos4f;
    FARPROC glRasterPos4fv;
    FARPROC glRasterPos4i;
    FARPROC glRasterPos4iv;
    FARPROC glRasterPos4s;
    FARPROC glRasterPos4sv;
    FARPROC glReadBuffer;
    FARPROC glReadPixels;
    FARPROC glRectd;
    FARPROC glRectdv;
    FARPROC glRectf;
    FARPROC glRectfv;
    FARPROC glRecti;
    FARPROC glRectiv;
    FARPROC glRects;
    FARPROC glRectsv;
    FARPROC glRenderMode;
    FARPROC glRotated;
    FARPROC glRotatef;
    FARPROC glScaled;
    FARPROC glScalef;
    FARPROC glScissor;
    FARPROC glSelectBuffer;
    FARPROC glShadeModel;
    FARPROC glStencilFunc;
    FARPROC glStencilMask;
    FARPROC glStencilOp;
    FARPROC glTexCoord1d;
    FARPROC glTexCoord1dv;
    FARPROC glTexCoord1f;
    FARPROC glTexCoord1fv;
    FARPROC glTexCoord1i;
    FARPROC glTexCoord1iv;
    FARPROC glTexCoord1s;
    FARPROC glTexCoord1sv;
    FARPROC glTexCoord2d;
    FARPROC glTexCoord2dv;
    FARPROC glTexCoord2f;
    FARPROC glTexCoord2fv;
    FARPROC glTexCoord2i;
    FARPROC glTexCoord2iv;
    FARPROC glTexCoord2s;
    FARPROC glTexCoord2sv;
    FARPROC glTexCoord3d;
    FARPROC glTexCoord3dv;
    FARPROC glTexCoord3f;
    FARPROC glTexCoord3fv;
    FARPROC glTexCoord3i;
    FARPROC glTexCoord3iv;
    FARPROC glTexCoord3s;
    FARPROC glTexCoord3sv;
    FARPROC glTexCoord4d;
    FARPROC glTexCoord4dv;
    FARPROC glTexCoord4f;
    FARPROC glTexCoord4fv;
    FARPROC glTexCoord4i;
    FARPROC glTexCoord4iv;
    FARPROC glTexCoord4s;
    FARPROC glTexCoord4sv;
    FARPROC glTexCoordPointer;
    FARPROC glTexEnvf;
    FARPROC glTexEnvfv;
    FARPROC glTexEnvi;
    FARPROC glTexEnviv;
    FARPROC glTexGend;
    FARPROC glTexGendv;
    FARPROC glTexGenf;
    FARPROC glTexGenfv;
    FARPROC glTexGeni;
    FARPROC glTexGeniv;
    FARPROC glTexImage1D;
    FARPROC glTexImage2D;
    FARPROC glTexParameterf;
    FARPROC glTexParameterfv;
    FARPROC glTexParameteri;
    FARPROC glTexParameteriv;
    FARPROC glTexSubImage1D;
    FARPROC glTexSubImage2D;
    FARPROC glTranslated;
    FARPROC glTranslatef;
    FARPROC glVertex2d;
    FARPROC glVertex2dv;
    FARPROC glVertex2f;
    FARPROC glVertex2fv;
    FARPROC glVertex2i;
    FARPROC glVertex2iv;
    FARPROC glVertex2s;
    FARPROC glVertex2sv;
    FARPROC glVertex3d;
    FARPROC glVertex3dv;
    FARPROC glVertex3f;
    FARPROC glVertex3fv;
    FARPROC glVertex3i;
    FARPROC glVertex3iv;
    FARPROC glVertex3s;
    FARPROC glVertex3sv;
    FARPROC glVertex4d;
    FARPROC glVertex4dv;
    FARPROC glVertex4f;
    FARPROC glVertex4fv;
    FARPROC glVertex4i;
    FARPROC glVertex4iv;
    FARPROC glVertex4s;
    FARPROC glVertex4sv;
    FARPROC glVertexPointer;
    FARPROC glViewport;
    FARPROC wglChoosePixelFormat;
    FARPROC wglCopyContext;
    FARPROC wglCreateContext;
    FARPROC wglCreateLayerContext;
    FARPROC wglDeleteContext;
    FARPROC wglDescribeLayerPlane;
    FARPROC wglDescribePixelFormat;
    FARPROC wglGetCurrentContext;
    FARPROC wglGetCurrentDC;
    FARPROC wglGetDefaultProcAddress;
    FARPROC wglGetLayerPaletteEntries;
    FARPROC wglGetPixelFormat;
    FARPROC wglGetProcAddress;
    FARPROC wglMakeCurrent;
    FARPROC wglRealizeLayerPalette;
    FARPROC wglSetLayerPaletteEntries;
    FARPROC wglSetPixelFormat;
    FARPROC wglShareLists;
    FARPROC wglSwapBuffers;
    FARPROC wglSwapLayerBuffers;
    FARPROC wglSwapMultipleBuffers;
    FARPROC wglUseFontBitmapsA;
    FARPROC wglUseFontBitmapsW;
    FARPROC wglUseFontOutlinesA;
    FARPROC wglUseFontOutlinesW;

    void LoadOriginalLibrary(HMODULE module)
    {
        dll = module;

        GlmfBeginGlsBlock = KernelBaseProxy::GetProcAddress_()(dll, "GlmfBeginGlsBlock");
        GlmfCloseMetaFile = KernelBaseProxy::GetProcAddress_()(dll, "GlmfCloseMetaFile");
        GlmfEndGlsBlock = KernelBaseProxy::GetProcAddress_()(dll, "GlmfEndGlsBlock");
        GlmfEndPlayback = KernelBaseProxy::GetProcAddress_()(dll, "GlmfEndPlayback");
        GlmfInitPlayback = KernelBaseProxy::GetProcAddress_()(dll, "GlmfInitPlayback");
        GlmfPlayGlsRecord = KernelBaseProxy::GetProcAddress_()(dll, "GlmfPlayGlsRecord");
        glAccum = KernelBaseProxy::GetProcAddress_()(dll, "glAccum");
        glAlphaFunc = KernelBaseProxy::GetProcAddress_()(dll, "glAlphaFunc");
        glAreTexturesResident = KernelBaseProxy::GetProcAddress_()(dll, "glAreTexturesResident");
        glArrayElement = KernelBaseProxy::GetProcAddress_()(dll, "glArrayElement");
        glBegin = KernelBaseProxy::GetProcAddress_()(dll, "glBegin");
        glBindTexture = KernelBaseProxy::GetProcAddress_()(dll, "glBindTexture");
        glBitmap = KernelBaseProxy::GetProcAddress_()(dll, "glBitmap");
        glBlendFunc = KernelBaseProxy::GetProcAddress_()(dll, "glBlendFunc");
        glCallList = KernelBaseProxy::GetProcAddress_()(dll, "glCallList");
        glCallLists = KernelBaseProxy::GetProcAddress_()(dll, "glCallLists");
        glClear = KernelBaseProxy::GetProcAddress_()(dll, "glClear");
        glClearAccum = KernelBaseProxy::GetProcAddress_()(dll, "glClearAccum");
        glClearColor = KernelBaseProxy::GetProcAddress_()(dll, "glClearColor");
        glClearDepth = KernelBaseProxy::GetProcAddress_()(dll, "glClearDepth");
        glClearIndex = KernelBaseProxy::GetProcAddress_()(dll, "glClearIndex");
        glClearStencil = KernelBaseProxy::GetProcAddress_()(dll, "glClearStencil");
        glClipPlane = KernelBaseProxy::GetProcAddress_()(dll, "glClipPlane");
        glColor3b = KernelBaseProxy::GetProcAddress_()(dll, "glColor3b");
        glColor3bv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3bv");
        glColor3d = KernelBaseProxy::GetProcAddress_()(dll, "glColor3d");
        glColor3dv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3dv");
        glColor3f = KernelBaseProxy::GetProcAddress_()(dll, "glColor3f");
        glColor3fv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3fv");
        glColor3i = KernelBaseProxy::GetProcAddress_()(dll, "glColor3i");
        glColor3iv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3iv");
        glColor3s = KernelBaseProxy::GetProcAddress_()(dll, "glColor3s");
        glColor3sv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3sv");
        glColor3ub = KernelBaseProxy::GetProcAddress_()(dll, "glColor3ub");
        glColor3ubv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3ubv");
        glColor3ui = KernelBaseProxy::GetProcAddress_()(dll, "glColor3ui");
        glColor3uiv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3uiv");
        glColor3us = KernelBaseProxy::GetProcAddress_()(dll, "glColor3us");
        glColor3usv = KernelBaseProxy::GetProcAddress_()(dll, "glColor3usv");
        glColor4b = KernelBaseProxy::GetProcAddress_()(dll, "glColor4b");
        glColor4bv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4bv");
        glColor4d = KernelBaseProxy::GetProcAddress_()(dll, "glColor4d");
        glColor4dv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4dv");
        glColor4f = KernelBaseProxy::GetProcAddress_()(dll, "glColor4f");
        glColor4fv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4fv");
        glColor4i = KernelBaseProxy::GetProcAddress_()(dll, "glColor4i");
        glColor4iv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4iv");
        glColor4s = KernelBaseProxy::GetProcAddress_()(dll, "glColor4s");
        glColor4sv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4sv");
        glColor4ub = KernelBaseProxy::GetProcAddress_()(dll, "glColor4ub");
        glColor4ubv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4ubv");
        glColor4ui = KernelBaseProxy::GetProcAddress_()(dll, "glColor4ui");
        glColor4uiv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4uiv");
        glColor4us = KernelBaseProxy::GetProcAddress_()(dll, "glColor4us");
        glColor4usv = KernelBaseProxy::GetProcAddress_()(dll, "glColor4usv");
        glColorMask = KernelBaseProxy::GetProcAddress_()(dll, "glColorMask");
        glColorMaterial = KernelBaseProxy::GetProcAddress_()(dll, "glColorMaterial");
        glColorPointer = KernelBaseProxy::GetProcAddress_()(dll, "glColorPointer");
        glCopyPixels = KernelBaseProxy::GetProcAddress_()(dll, "glCopyPixels");
        glCopyTexImage1D = KernelBaseProxy::GetProcAddress_()(dll, "glCopyTexImage1D");
        glCopyTexImage2D = KernelBaseProxy::GetProcAddress_()(dll, "glCopyTexImage2D");
        glCopyTexSubImage1D = KernelBaseProxy::GetProcAddress_()(dll, "glCopyTexSubImage1D");
        glCopyTexSubImage2D = KernelBaseProxy::GetProcAddress_()(dll, "glCopyTexSubImage2D");
        glCullFace = KernelBaseProxy::GetProcAddress_()(dll, "glCullFace");
        glDebugEntry = KernelBaseProxy::GetProcAddress_()(dll, "glDebugEntry");
        glDeleteLists = KernelBaseProxy::GetProcAddress_()(dll, "glDeleteLists");
        glDeleteTextures = KernelBaseProxy::GetProcAddress_()(dll, "glDeleteTextures");
        glDepthFunc = KernelBaseProxy::GetProcAddress_()(dll, "glDepthFunc");
        glDepthMask = KernelBaseProxy::GetProcAddress_()(dll, "glDepthMask");
        glDepthRange = KernelBaseProxy::GetProcAddress_()(dll, "glDepthRange");
        glDisable = KernelBaseProxy::GetProcAddress_()(dll, "glDisable");
        glDisableClientState = KernelBaseProxy::GetProcAddress_()(dll, "glDisableClientState");
        glDrawArrays = KernelBaseProxy::GetProcAddress_()(dll, "glDrawArrays");
        glDrawBuffer = KernelBaseProxy::GetProcAddress_()(dll, "glDrawBuffer");
        glDrawElements = KernelBaseProxy::GetProcAddress_()(dll, "glDrawElements");
        glDrawPixels = KernelBaseProxy::GetProcAddress_()(dll, "glDrawPixels");
        glEdgeFlag = KernelBaseProxy::GetProcAddress_()(dll, "glEdgeFlag");
        glEdgeFlagPointer = KernelBaseProxy::GetProcAddress_()(dll, "glEdgeFlagPointer");
        glEdgeFlagv = KernelBaseProxy::GetProcAddress_()(dll, "glEdgeFlagv");
        glEnable = KernelBaseProxy::GetProcAddress_()(dll, "glEnable");
        glEnableClientState = KernelBaseProxy::GetProcAddress_()(dll, "glEnableClientState");
        glEnd = KernelBaseProxy::GetProcAddress_()(dll, "glEnd");
        glEndList = KernelBaseProxy::GetProcAddress_()(dll, "glEndList");
        glEvalCoord1d = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord1d");
        glEvalCoord1dv = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord1dv");
        glEvalCoord1f = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord1f");
        glEvalCoord1fv = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord1fv");
        glEvalCoord2d = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord2d");
        glEvalCoord2dv = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord2dv");
        glEvalCoord2f = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord2f");
        glEvalCoord2fv = KernelBaseProxy::GetProcAddress_()(dll, "glEvalCoord2fv");
        glEvalMesh1 = KernelBaseProxy::GetProcAddress_()(dll, "glEvalMesh1");
        glEvalMesh2 = KernelBaseProxy::GetProcAddress_()(dll, "glEvalMesh2");
        glEvalPoint1 = KernelBaseProxy::GetProcAddress_()(dll, "glEvalPoint1");
        glEvalPoint2 = KernelBaseProxy::GetProcAddress_()(dll, "glEvalPoint2");
        glFeedbackBuffer = KernelBaseProxy::GetProcAddress_()(dll, "glFeedbackBuffer");
        glFinish = KernelBaseProxy::GetProcAddress_()(dll, "glFinish");
        glFlush = KernelBaseProxy::GetProcAddress_()(dll, "glFlush");
        glFogf = KernelBaseProxy::GetProcAddress_()(dll, "glFogf");
        glFogfv = KernelBaseProxy::GetProcAddress_()(dll, "glFogfv");
        glFogi = KernelBaseProxy::GetProcAddress_()(dll, "glFogi");
        glFogiv = KernelBaseProxy::GetProcAddress_()(dll, "glFogiv");
        glFrontFace = KernelBaseProxy::GetProcAddress_()(dll, "glFrontFace");
        glFrustum = KernelBaseProxy::GetProcAddress_()(dll, "glFrustum");
        glGenLists = KernelBaseProxy::GetProcAddress_()(dll, "glGenLists");
        glGenTextures = KernelBaseProxy::GetProcAddress_()(dll, "glGenTextures");
        glGetBooleanv = KernelBaseProxy::GetProcAddress_()(dll, "glGetBooleanv");
        glGetClipPlane = KernelBaseProxy::GetProcAddress_()(dll, "glGetClipPlane");
        glGetDoublev = KernelBaseProxy::GetProcAddress_()(dll, "glGetDoublev");
        glGetError = KernelBaseProxy::GetProcAddress_()(dll, "glGetError");
        glGetFloatv = KernelBaseProxy::GetProcAddress_()(dll, "glGetFloatv");
        glGetIntegerv = KernelBaseProxy::GetProcAddress_()(dll, "glGetIntegerv");
        glGetLightfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetLightfv");
        glGetLightiv = KernelBaseProxy::GetProcAddress_()(dll, "glGetLightiv");
        glGetMapdv = KernelBaseProxy::GetProcAddress_()(dll, "glGetMapdv");
        glGetMapfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetMapfv");
        glGetMapiv = KernelBaseProxy::GetProcAddress_()(dll, "glGetMapiv");
        glGetMaterialfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetMaterialfv");
        glGetMaterialiv = KernelBaseProxy::GetProcAddress_()(dll, "glGetMaterialiv");
        glGetPixelMapfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetPixelMapfv");
        glGetPixelMapuiv = KernelBaseProxy::GetProcAddress_()(dll, "glGetPixelMapuiv");
        glGetPixelMapusv = KernelBaseProxy::GetProcAddress_()(dll, "glGetPixelMapusv");
        glGetPointerv = KernelBaseProxy::GetProcAddress_()(dll, "glGetPointerv");
        glGetPolygonStipple = KernelBaseProxy::GetProcAddress_()(dll, "glGetPolygonStipple");
        glGetString = KernelBaseProxy::GetProcAddress_()(dll, "glGetString");
        glGetTexEnvfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexEnvfv");
        glGetTexEnviv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexEnviv");
        glGetTexGendv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexGendv");
        glGetTexGenfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexGenfv");
        glGetTexGeniv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexGeniv");
        glGetTexImage = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexImage");
        glGetTexLevelParameterfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexLevelParameterfv");
        glGetTexLevelParameteriv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexLevelParameteriv");
        glGetTexParameterfv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexParameterfv");
        glGetTexParameteriv = KernelBaseProxy::GetProcAddress_()(dll, "glGetTexParameteriv");
        glHint = KernelBaseProxy::GetProcAddress_()(dll, "glHint");
        glIndexMask = KernelBaseProxy::GetProcAddress_()(dll, "glIndexMask");
        glIndexPointer = KernelBaseProxy::GetProcAddress_()(dll, "glIndexPointer");
        glIndexd = KernelBaseProxy::GetProcAddress_()(dll, "glIndexd");
        glIndexdv = KernelBaseProxy::GetProcAddress_()(dll, "glIndexdv");
        glIndexf = KernelBaseProxy::GetProcAddress_()(dll, "glIndexf");
        glIndexfv = KernelBaseProxy::GetProcAddress_()(dll, "glIndexfv");
        glIndexi = KernelBaseProxy::GetProcAddress_()(dll, "glIndexi");
        glIndexiv = KernelBaseProxy::GetProcAddress_()(dll, "glIndexiv");
        glIndexs = KernelBaseProxy::GetProcAddress_()(dll, "glIndexs");
        glIndexsv = KernelBaseProxy::GetProcAddress_()(dll, "glIndexsv");
        glIndexub = KernelBaseProxy::GetProcAddress_()(dll, "glIndexub");
        glIndexubv = KernelBaseProxy::GetProcAddress_()(dll, "glIndexubv");
        glInitNames = KernelBaseProxy::GetProcAddress_()(dll, "glInitNames");
        glInterleavedArrays = KernelBaseProxy::GetProcAddress_()(dll, "glInterleavedArrays");
        glIsEnabled = KernelBaseProxy::GetProcAddress_()(dll, "glIsEnabled");
        glIsList = KernelBaseProxy::GetProcAddress_()(dll, "glIsList");
        glIsTexture = KernelBaseProxy::GetProcAddress_()(dll, "glIsTexture");
        glLightModelf = KernelBaseProxy::GetProcAddress_()(dll, "glLightModelf");
        glLightModelfv = KernelBaseProxy::GetProcAddress_()(dll, "glLightModelfv");
        glLightModeli = KernelBaseProxy::GetProcAddress_()(dll, "glLightModeli");
        glLightModeliv = KernelBaseProxy::GetProcAddress_()(dll, "glLightModeliv");
        glLightf = KernelBaseProxy::GetProcAddress_()(dll, "glLightf");
        glLightfv = KernelBaseProxy::GetProcAddress_()(dll, "glLightfv");
        glLighti = KernelBaseProxy::GetProcAddress_()(dll, "glLighti");
        glLightiv = KernelBaseProxy::GetProcAddress_()(dll, "glLightiv");
        glLineStipple = KernelBaseProxy::GetProcAddress_()(dll, "glLineStipple");
        glLineWidth = KernelBaseProxy::GetProcAddress_()(dll, "glLineWidth");
        glListBase = KernelBaseProxy::GetProcAddress_()(dll, "glListBase");
        glLoadIdentity = KernelBaseProxy::GetProcAddress_()(dll, "glLoadIdentity");
        glLoadMatrixd = KernelBaseProxy::GetProcAddress_()(dll, "glLoadMatrixd");
        glLoadMatrixf = KernelBaseProxy::GetProcAddress_()(dll, "glLoadMatrixf");
        glLoadName = KernelBaseProxy::GetProcAddress_()(dll, "glLoadName");
        glLogicOp = KernelBaseProxy::GetProcAddress_()(dll, "glLogicOp");
        glMap1d = KernelBaseProxy::GetProcAddress_()(dll, "glMap1d");
        glMap1f = KernelBaseProxy::GetProcAddress_()(dll, "glMap1f");
        glMap2d = KernelBaseProxy::GetProcAddress_()(dll, "glMap2d");
        glMap2f = KernelBaseProxy::GetProcAddress_()(dll, "glMap2f");
        glMapGrid1d = KernelBaseProxy::GetProcAddress_()(dll, "glMapGrid1d");
        glMapGrid1f = KernelBaseProxy::GetProcAddress_()(dll, "glMapGrid1f");
        glMapGrid2d = KernelBaseProxy::GetProcAddress_()(dll, "glMapGrid2d");
        glMapGrid2f = KernelBaseProxy::GetProcAddress_()(dll, "glMapGrid2f");
        glMaterialf = KernelBaseProxy::GetProcAddress_()(dll, "glMaterialf");
        glMaterialfv = KernelBaseProxy::GetProcAddress_()(dll, "glMaterialfv");
        glMateriali = KernelBaseProxy::GetProcAddress_()(dll, "glMateriali");
        glMaterialiv = KernelBaseProxy::GetProcAddress_()(dll, "glMaterialiv");
        glMatrixMode = KernelBaseProxy::GetProcAddress_()(dll, "glMatrixMode");
        glMultMatrixd = KernelBaseProxy::GetProcAddress_()(dll, "glMultMatrixd");
        glMultMatrixf = KernelBaseProxy::GetProcAddress_()(dll, "glMultMatrixf");
        glNewList = KernelBaseProxy::GetProcAddress_()(dll, "glNewList");
        glNormal3b = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3b");
        glNormal3bv = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3bv");
        glNormal3d = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3d");
        glNormal3dv = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3dv");
        glNormal3f = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3f");
        glNormal3fv = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3fv");
        glNormal3i = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3i");
        glNormal3iv = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3iv");
        glNormal3s = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3s");
        glNormal3sv = KernelBaseProxy::GetProcAddress_()(dll, "glNormal3sv");
        glNormalPointer = KernelBaseProxy::GetProcAddress_()(dll, "glNormalPointer");
        glOrtho = KernelBaseProxy::GetProcAddress_()(dll, "glOrtho");
        glPassThrough = KernelBaseProxy::GetProcAddress_()(dll, "glPassThrough");
        glPixelMapfv = KernelBaseProxy::GetProcAddress_()(dll, "glPixelMapfv");
        glPixelMapuiv = KernelBaseProxy::GetProcAddress_()(dll, "glPixelMapuiv");
        glPixelMapusv = KernelBaseProxy::GetProcAddress_()(dll, "glPixelMapusv");
        glPixelStoref = KernelBaseProxy::GetProcAddress_()(dll, "glPixelStoref");
        glPixelStorei = KernelBaseProxy::GetProcAddress_()(dll, "glPixelStorei");
        glPixelTransferf = KernelBaseProxy::GetProcAddress_()(dll, "glPixelTransferf");
        glPixelTransferi = KernelBaseProxy::GetProcAddress_()(dll, "glPixelTransferi");
        glPixelZoom = KernelBaseProxy::GetProcAddress_()(dll, "glPixelZoom");
        glPointSize = KernelBaseProxy::GetProcAddress_()(dll, "glPointSize");
        glPolygonMode = KernelBaseProxy::GetProcAddress_()(dll, "glPolygonMode");
        glPolygonOffset = KernelBaseProxy::GetProcAddress_()(dll, "glPolygonOffset");
        glPolygonStipple = KernelBaseProxy::GetProcAddress_()(dll, "glPolygonStipple");
        glPopAttrib = KernelBaseProxy::GetProcAddress_()(dll, "glPopAttrib");
        glPopClientAttrib = KernelBaseProxy::GetProcAddress_()(dll, "glPopClientAttrib");
        glPopMatrix = KernelBaseProxy::GetProcAddress_()(dll, "glPopMatrix");
        glPopName = KernelBaseProxy::GetProcAddress_()(dll, "glPopName");
        glPrioritizeTextures = KernelBaseProxy::GetProcAddress_()(dll, "glPrioritizeTextures");
        glPushAttrib = KernelBaseProxy::GetProcAddress_()(dll, "glPushAttrib");
        glPushClientAttrib = KernelBaseProxy::GetProcAddress_()(dll, "glPushClientAttrib");
        glPushMatrix = KernelBaseProxy::GetProcAddress_()(dll, "glPushMatrix");
        glPushName = KernelBaseProxy::GetProcAddress_()(dll, "glPushName");
        glRasterPos2d = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2d");
        glRasterPos2dv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2dv");
        glRasterPos2f = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2f");
        glRasterPos2fv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2fv");
        glRasterPos2i = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2i");
        glRasterPos2iv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2iv");
        glRasterPos2s = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2s");
        glRasterPos2sv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos2sv");
        glRasterPos3d = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3d");
        glRasterPos3dv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3dv");
        glRasterPos3f = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3f");
        glRasterPos3fv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3fv");
        glRasterPos3i = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3i");
        glRasterPos3iv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3iv");
        glRasterPos3s = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3s");
        glRasterPos3sv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos3sv");
        glRasterPos4d = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4d");
        glRasterPos4dv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4dv");
        glRasterPos4f = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4f");
        glRasterPos4fv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4fv");
        glRasterPos4i = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4i");
        glRasterPos4iv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4iv");
        glRasterPos4s = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4s");
        glRasterPos4sv = KernelBaseProxy::GetProcAddress_()(dll, "glRasterPos4sv");
        glReadBuffer = KernelBaseProxy::GetProcAddress_()(dll, "glReadBuffer");
        glReadPixels = KernelBaseProxy::GetProcAddress_()(dll, "glReadPixels");
        glRectd = KernelBaseProxy::GetProcAddress_()(dll, "glRectd");
        glRectdv = KernelBaseProxy::GetProcAddress_()(dll, "glRectdv");
        glRectf = KernelBaseProxy::GetProcAddress_()(dll, "glRectf");
        glRectfv = KernelBaseProxy::GetProcAddress_()(dll, "glRectfv");
        glRecti = KernelBaseProxy::GetProcAddress_()(dll, "glRecti");
        glRectiv = KernelBaseProxy::GetProcAddress_()(dll, "glRectiv");
        glRects = KernelBaseProxy::GetProcAddress_()(dll, "glRects");
        glRectsv = KernelBaseProxy::GetProcAddress_()(dll, "glRectsv");
        glRenderMode = KernelBaseProxy::GetProcAddress_()(dll, "glRenderMode");
        glRotated = KernelBaseProxy::GetProcAddress_()(dll, "glRotated");
        glRotatef = KernelBaseProxy::GetProcAddress_()(dll, "glRotatef");
        glScaled = KernelBaseProxy::GetProcAddress_()(dll, "glScaled");
        glScalef = KernelBaseProxy::GetProcAddress_()(dll, "glScalef");
        glScissor = KernelBaseProxy::GetProcAddress_()(dll, "glScissor");
        glSelectBuffer = KernelBaseProxy::GetProcAddress_()(dll, "glSelectBuffer");
        glShadeModel = KernelBaseProxy::GetProcAddress_()(dll, "glShadeModel");
        glStencilFunc = KernelBaseProxy::GetProcAddress_()(dll, "glStencilFunc");
        glStencilMask = KernelBaseProxy::GetProcAddress_()(dll, "glStencilMask");
        glStencilOp = KernelBaseProxy::GetProcAddress_()(dll, "glStencilOp");
        glTexCoord1d = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1d");
        glTexCoord1dv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1dv");
        glTexCoord1f = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1f");
        glTexCoord1fv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1fv");
        glTexCoord1i = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1i");
        glTexCoord1iv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1iv");
        glTexCoord1s = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1s");
        glTexCoord1sv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord1sv");
        glTexCoord2d = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2d");
        glTexCoord2dv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2dv");
        glTexCoord2f = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2f");
        glTexCoord2fv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2fv");
        glTexCoord2i = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2i");
        glTexCoord2iv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2iv");
        glTexCoord2s = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2s");
        glTexCoord2sv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord2sv");
        glTexCoord3d = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3d");
        glTexCoord3dv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3dv");
        glTexCoord3f = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3f");
        glTexCoord3fv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3fv");
        glTexCoord3i = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3i");
        glTexCoord3iv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3iv");
        glTexCoord3s = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3s");
        glTexCoord3sv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord3sv");
        glTexCoord4d = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4d");
        glTexCoord4dv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4dv");
        glTexCoord4f = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4f");
        glTexCoord4fv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4fv");
        glTexCoord4i = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4i");
        glTexCoord4iv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4iv");
        glTexCoord4s = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4s");
        glTexCoord4sv = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoord4sv");
        glTexCoordPointer = KernelBaseProxy::GetProcAddress_()(dll, "glTexCoordPointer");
        glTexEnvf = KernelBaseProxy::GetProcAddress_()(dll, "glTexEnvf");
        glTexEnvfv = KernelBaseProxy::GetProcAddress_()(dll, "glTexEnvfv");
        glTexEnvi = KernelBaseProxy::GetProcAddress_()(dll, "glTexEnvi");
        glTexEnviv = KernelBaseProxy::GetProcAddress_()(dll, "glTexEnviv");
        glTexGend = KernelBaseProxy::GetProcAddress_()(dll, "glTexGend");
        glTexGendv = KernelBaseProxy::GetProcAddress_()(dll, "glTexGendv");
        glTexGenf = KernelBaseProxy::GetProcAddress_()(dll, "glTexGenf");
        glTexGenfv = KernelBaseProxy::GetProcAddress_()(dll, "glTexGenfv");
        glTexGeni = KernelBaseProxy::GetProcAddress_()(dll, "glTexGeni");
        glTexGeniv = KernelBaseProxy::GetProcAddress_()(dll, "glTexGeniv");
        glTexImage1D = KernelBaseProxy::GetProcAddress_()(dll, "glTexImage1D");
        glTexImage2D = KernelBaseProxy::GetProcAddress_()(dll, "glTexImage2D");
        glTexParameterf = KernelBaseProxy::GetProcAddress_()(dll, "glTexParameterf");
        glTexParameterfv = KernelBaseProxy::GetProcAddress_()(dll, "glTexParameterfv");
        glTexParameteri = KernelBaseProxy::GetProcAddress_()(dll, "glTexParameteri");
        glTexParameteriv = KernelBaseProxy::GetProcAddress_()(dll, "glTexParameteriv");
        glTexSubImage1D = KernelBaseProxy::GetProcAddress_()(dll, "glTexSubImage1D");
        glTexSubImage2D = KernelBaseProxy::GetProcAddress_()(dll, "glTexSubImage2D");
        glTranslated = KernelBaseProxy::GetProcAddress_()(dll, "glTranslated");
        glTranslatef = KernelBaseProxy::GetProcAddress_()(dll, "glTranslatef");
        glVertex2d = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2d");
        glVertex2dv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2dv");
        glVertex2f = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2f");
        glVertex2fv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2fv");
        glVertex2i = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2i");
        glVertex2iv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2iv");
        glVertex2s = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2s");
        glVertex2sv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex2sv");
        glVertex3d = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3d");
        glVertex3dv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3dv");
        glVertex3f = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3f");
        glVertex3fv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3fv");
        glVertex3i = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3i");
        glVertex3iv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3iv");
        glVertex3s = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3s");
        glVertex3sv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex3sv");
        glVertex4d = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4d");
        glVertex4dv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4dv");
        glVertex4f = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4f");
        glVertex4fv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4fv");
        glVertex4i = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4i");
        glVertex4iv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4iv");
        glVertex4s = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4s");
        glVertex4sv = KernelBaseProxy::GetProcAddress_()(dll, "glVertex4sv");
        glVertexPointer = KernelBaseProxy::GetProcAddress_()(dll, "glVertexPointer");
        glViewport = KernelBaseProxy::GetProcAddress_()(dll, "glViewport");
        wglChoosePixelFormat = KernelBaseProxy::GetProcAddress_()(dll, "wglChoosePixelFormat");
        wglCopyContext = KernelBaseProxy::GetProcAddress_()(dll, "wglCopyContext");
        wglCreateContext = KernelBaseProxy::GetProcAddress_()(dll, "wglCreateContext");
        wglCreateLayerContext = KernelBaseProxy::GetProcAddress_()(dll, "wglCreateLayerContext");
        wglDeleteContext = KernelBaseProxy::GetProcAddress_()(dll, "wglDeleteContext");
        wglDescribeLayerPlane = KernelBaseProxy::GetProcAddress_()(dll, "wglDescribeLayerPlane");
        wglDescribePixelFormat = KernelBaseProxy::GetProcAddress_()(dll, "wglDescribePixelFormat");
        wglGetCurrentContext = KernelBaseProxy::GetProcAddress_()(dll, "wglGetCurrentContext");
        wglGetCurrentDC = KernelBaseProxy::GetProcAddress_()(dll, "wglGetCurrentDC");
        wglGetDefaultProcAddress = KernelBaseProxy::GetProcAddress_()(dll, "wglGetDefaultProcAddress");
        wglGetLayerPaletteEntries = KernelBaseProxy::GetProcAddress_()(dll, "wglGetLayerPaletteEntries");
        wglGetPixelFormat = KernelBaseProxy::GetProcAddress_()(dll, "wglGetPixelFormat");
        wglGetProcAddress = KernelBaseProxy::GetProcAddress_()(dll, "wglGetProcAddress");
        wglMakeCurrent = KernelBaseProxy::GetProcAddress_()(dll, "wglMakeCurrent");
        wglRealizeLayerPalette = KernelBaseProxy::GetProcAddress_()(dll, "wglRealizeLayerPalette");
        wglSetLayerPaletteEntries = KernelBaseProxy::GetProcAddress_()(dll, "wglSetLayerPaletteEntries");
        wglSetPixelFormat = KernelBaseProxy::GetProcAddress_()(dll, "wglSetPixelFormat");
        wglShareLists = KernelBaseProxy::GetProcAddress_()(dll, "wglShareLists");
        wglSwapBuffers = KernelBaseProxy::GetProcAddress_()(dll, "wglSwapBuffers");
        wglSwapLayerBuffers = KernelBaseProxy::GetProcAddress_()(dll, "wglSwapLayerBuffers");
        wglSwapMultipleBuffers = KernelBaseProxy::GetProcAddress_()(dll, "wglSwapMultipleBuffers");
        wglUseFontBitmapsA = KernelBaseProxy::GetProcAddress_()(dll, "wglUseFontBitmapsA");
        wglUseFontBitmapsW = KernelBaseProxy::GetProcAddress_()(dll, "wglUseFontBitmapsW");
        wglUseFontOutlinesA = KernelBaseProxy::GetProcAddress_()(dll, "wglUseFontOutlinesA");
        wglUseFontOutlinesW = KernelBaseProxy::GetProcAddress_()(dll, "wglUseFontOutlinesW");
    }
} opengl32;

void _GlmfBeginGlsBlock() { opengl32.GlmfBeginGlsBlock(); }
void _GlmfCloseMetaFile() { opengl32.GlmfCloseMetaFile(); }
void _GlmfEndGlsBlock() { opengl32.GlmfEndGlsBlock(); }
void _GlmfEndPlayback() { opengl32.GlmfEndPlayback(); }
void _GlmfInitPlayback() { opengl32.GlmfInitPlayback(); }
void _GlmfPlayGlsRecord() { opengl32.GlmfPlayGlsRecord(); }
void _glAccum() { opengl32.glAccum(); }
void _glAlphaFunc() { opengl32.glAlphaFunc(); }
void _glAreTexturesResident() { opengl32.glAreTexturesResident(); }
void _glArrayElement() { opengl32.glArrayElement(); }
void _glBegin() { opengl32.glBegin(); }
void _glBindTexture() { opengl32.glBindTexture(); }
void _glBitmap() { opengl32.glBitmap(); }
void _glBlendFunc() { opengl32.glBlendFunc(); }
void _glCallList() { opengl32.glCallList(); }
void _glCallLists() { opengl32.glCallLists(); }
void _glClear() { opengl32.glClear(); }
void _glClearAccum() { opengl32.glClearAccum(); }
void _glClearColor() { opengl32.glClearColor(); }
void _glClearDepth() { opengl32.glClearDepth(); }
void _glClearIndex() { opengl32.glClearIndex(); }
void _glClearStencil() { opengl32.glClearStencil(); }
void _glClipPlane() { opengl32.glClipPlane(); }
void _glColor3b() { opengl32.glColor3b(); }
void _glColor3bv() { opengl32.glColor3bv(); }
void _glColor3d() { opengl32.glColor3d(); }
void _glColor3dv() { opengl32.glColor3dv(); }
void _glColor3f() { opengl32.glColor3f(); }
void _glColor3fv() { opengl32.glColor3fv(); }
void _glColor3i() { opengl32.glColor3i(); }
void _glColor3iv() { opengl32.glColor3iv(); }
void _glColor3s() { opengl32.glColor3s(); }
void _glColor3sv() { opengl32.glColor3sv(); }
void _glColor3ub() { opengl32.glColor3ub(); }
void _glColor3ubv() { opengl32.glColor3ubv(); }
void _glColor3ui() { opengl32.glColor3ui(); }
void _glColor3uiv() { opengl32.glColor3uiv(); }
void _glColor3us() { opengl32.glColor3us(); }
void _glColor3usv() { opengl32.glColor3usv(); }
void _glColor4b() { opengl32.glColor4b(); }
void _glColor4bv() { opengl32.glColor4bv(); }
void _glColor4d() { opengl32.glColor4d(); }
void _glColor4dv() { opengl32.glColor4dv(); }
void _glColor4f() { opengl32.glColor4f(); }
void _glColor4fv() { opengl32.glColor4fv(); }
void _glColor4i() { opengl32.glColor4i(); }
void _glColor4iv() { opengl32.glColor4iv(); }
void _glColor4s() { opengl32.glColor4s(); }
void _glColor4sv() { opengl32.glColor4sv(); }
void _glColor4ub() { opengl32.glColor4ub(); }
void _glColor4ubv() { opengl32.glColor4ubv(); }
void _glColor4ui() { opengl32.glColor4ui(); }
void _glColor4uiv() { opengl32.glColor4uiv(); }
void _glColor4us() { opengl32.glColor4us(); }
void _glColor4usv() { opengl32.glColor4usv(); }
void _glColorMask() { opengl32.glColorMask(); }
void _glColorMaterial() { opengl32.glColorMaterial(); }
void _glColorPointer() { opengl32.glColorPointer(); }
void _glCopyPixels() { opengl32.glCopyPixels(); }
void _glCopyTexImage1D() { opengl32.glCopyTexImage1D(); }
void _glCopyTexImage2D() { opengl32.glCopyTexImage2D(); }
void _glCopyTexSubImage1D() { opengl32.glCopyTexSubImage1D(); }
void _glCopyTexSubImage2D() { opengl32.glCopyTexSubImage2D(); }
void _glCullFace() { opengl32.glCullFace(); }
void _glDebugEntry() { opengl32.glDebugEntry(); }
void _glDeleteLists() { opengl32.glDeleteLists(); }
void _glDeleteTextures() { opengl32.glDeleteTextures(); }
void _glDepthFunc() { opengl32.glDepthFunc(); }
void _glDepthMask() { opengl32.glDepthMask(); }
void _glDepthRange() { opengl32.glDepthRange(); }
void _glDisable() { opengl32.glDisable(); }
void _glDisableClientState() { opengl32.glDisableClientState(); }
void _glDrawArrays() { opengl32.glDrawArrays(); }
void _glDrawBuffer() { opengl32.glDrawBuffer(); }
void _glDrawElements() { opengl32.glDrawElements(); }
void _glDrawPixels() { opengl32.glDrawPixels(); }
void _glEdgeFlag() { opengl32.glEdgeFlag(); }
void _glEdgeFlagPointer() { opengl32.glEdgeFlagPointer(); }
void _glEdgeFlagv() { opengl32.glEdgeFlagv(); }
void _glEnable() { opengl32.glEnable(); }
void _glEnableClientState() { opengl32.glEnableClientState(); }
void _glEnd() { opengl32.glEnd(); }
void _glEndList() { opengl32.glEndList(); }
void _glEvalCoord1d() { opengl32.glEvalCoord1d(); }
void _glEvalCoord1dv() { opengl32.glEvalCoord1dv(); }
void _glEvalCoord1f() { opengl32.glEvalCoord1f(); }
void _glEvalCoord1fv() { opengl32.glEvalCoord1fv(); }
void _glEvalCoord2d() { opengl32.glEvalCoord2d(); }
void _glEvalCoord2dv() { opengl32.glEvalCoord2dv(); }
void _glEvalCoord2f() { opengl32.glEvalCoord2f(); }
void _glEvalCoord2fv() { opengl32.glEvalCoord2fv(); }
void _glEvalMesh1() { opengl32.glEvalMesh1(); }
void _glEvalMesh2() { opengl32.glEvalMesh2(); }
void _glEvalPoint1() { opengl32.glEvalPoint1(); }
void _glEvalPoint2() { opengl32.glEvalPoint2(); }
void _glFeedbackBuffer() { opengl32.glFeedbackBuffer(); }
void _glFinish() { opengl32.glFinish(); }
void _glFlush() { opengl32.glFlush(); }
void _glFogf() { opengl32.glFogf(); }
void _glFogfv() { opengl32.glFogfv(); }
void _glFogi() { opengl32.glFogi(); }
void _glFogiv() { opengl32.glFogiv(); }
void _glFrontFace() { opengl32.glFrontFace(); }
void _glFrustum() { opengl32.glFrustum(); }
void _glGenLists() { opengl32.glGenLists(); }
void _glGenTextures() { opengl32.glGenTextures(); }
void _glGetBooleanv() { opengl32.glGetBooleanv(); }
void _glGetClipPlane() { opengl32.glGetClipPlane(); }
void _glGetDoublev() { opengl32.glGetDoublev(); }
void _glGetError() { opengl32.glGetError(); }
void _glGetFloatv() { opengl32.glGetFloatv(); }
void _glGetIntegerv() { opengl32.glGetIntegerv(); }
void _glGetLightfv() { opengl32.glGetLightfv(); }
void _glGetLightiv() { opengl32.glGetLightiv(); }
void _glGetMapdv() { opengl32.glGetMapdv(); }
void _glGetMapfv() { opengl32.glGetMapfv(); }
void _glGetMapiv() { opengl32.glGetMapiv(); }
void _glGetMaterialfv() { opengl32.glGetMaterialfv(); }
void _glGetMaterialiv() { opengl32.glGetMaterialiv(); }
void _glGetPixelMapfv() { opengl32.glGetPixelMapfv(); }
void _glGetPixelMapuiv() { opengl32.glGetPixelMapuiv(); }
void _glGetPixelMapusv() { opengl32.glGetPixelMapusv(); }
void _glGetPointerv() { opengl32.glGetPointerv(); }
void _glGetPolygonStipple() { opengl32.glGetPolygonStipple(); }
void _glGetString() { opengl32.glGetString(); }
void _glGetTexEnvfv() { opengl32.glGetTexEnvfv(); }
void _glGetTexEnviv() { opengl32.glGetTexEnviv(); }
void _glGetTexGendv() { opengl32.glGetTexGendv(); }
void _glGetTexGenfv() { opengl32.glGetTexGenfv(); }
void _glGetTexGeniv() { opengl32.glGetTexGeniv(); }
void _glGetTexImage() { opengl32.glGetTexImage(); }
void _glGetTexLevelParameterfv() { opengl32.glGetTexLevelParameterfv(); }
void _glGetTexLevelParameteriv() { opengl32.glGetTexLevelParameteriv(); }
void _glGetTexParameterfv() { opengl32.glGetTexParameterfv(); }
void _glGetTexParameteriv() { opengl32.glGetTexParameteriv(); }
void _glHint() { opengl32.glHint(); }
void _glIndexMask() { opengl32.glIndexMask(); }
void _glIndexPointer() { opengl32.glIndexPointer(); }
void _glIndexd() { opengl32.glIndexd(); }
void _glIndexdv() { opengl32.glIndexdv(); }
void _glIndexf() { opengl32.glIndexf(); }
void _glIndexfv() { opengl32.glIndexfv(); }
void _glIndexi() { opengl32.glIndexi(); }
void _glIndexiv() { opengl32.glIndexiv(); }
void _glIndexs() { opengl32.glIndexs(); }
void _glIndexsv() { opengl32.glIndexsv(); }
void _glIndexub() { opengl32.glIndexub(); }
void _glIndexubv() { opengl32.glIndexubv(); }
void _glInitNames() { opengl32.glInitNames(); }
void _glInterleavedArrays() { opengl32.glInterleavedArrays(); }
void _glIsEnabled() { opengl32.glIsEnabled(); }
void _glIsList() { opengl32.glIsList(); }
void _glIsTexture() { opengl32.glIsTexture(); }
void _glLightModelf() { opengl32.glLightModelf(); }
void _glLightModelfv() { opengl32.glLightModelfv(); }
void _glLightModeli() { opengl32.glLightModeli(); }
void _glLightModeliv() { opengl32.glLightModeliv(); }
void _glLightf() { opengl32.glLightf(); }
void _glLightfv() { opengl32.glLightfv(); }
void _glLighti() { opengl32.glLighti(); }
void _glLightiv() { opengl32.glLightiv(); }
void _glLineStipple() { opengl32.glLineStipple(); }
void _glLineWidth() { opengl32.glLineWidth(); }
void _glListBase() { opengl32.glListBase(); }
void _glLoadIdentity() { opengl32.glLoadIdentity(); }
void _glLoadMatrixd() { opengl32.glLoadMatrixd(); }
void _glLoadMatrixf() { opengl32.glLoadMatrixf(); }
void _glLoadName() { opengl32.glLoadName(); }
void _glLogicOp() { opengl32.glLogicOp(); }
void _glMap1d() { opengl32.glMap1d(); }
void _glMap1f() { opengl32.glMap1f(); }
void _glMap2d() { opengl32.glMap2d(); }
void _glMap2f() { opengl32.glMap2f(); }
void _glMapGrid1d() { opengl32.glMapGrid1d(); }
void _glMapGrid1f() { opengl32.glMapGrid1f(); }
void _glMapGrid2d() { opengl32.glMapGrid2d(); }
void _glMapGrid2f() { opengl32.glMapGrid2f(); }
void _glMaterialf() { opengl32.glMaterialf(); }
void _glMaterialfv() { opengl32.glMaterialfv(); }
void _glMateriali() { opengl32.glMateriali(); }
void _glMaterialiv() { opengl32.glMaterialiv(); }
void _glMatrixMode() { opengl32.glMatrixMode(); }
void _glMultMatrixd() { opengl32.glMultMatrixd(); }
void _glMultMatrixf() { opengl32.glMultMatrixf(); }
void _glNewList() { opengl32.glNewList(); }
void _glNormal3b() { opengl32.glNormal3b(); }
void _glNormal3bv() { opengl32.glNormal3bv(); }
void _glNormal3d() { opengl32.glNormal3d(); }
void _glNormal3dv() { opengl32.glNormal3dv(); }
void _glNormal3f() { opengl32.glNormal3f(); }
void _glNormal3fv() { opengl32.glNormal3fv(); }
void _glNormal3i() { opengl32.glNormal3i(); }
void _glNormal3iv() { opengl32.glNormal3iv(); }
void _glNormal3s() { opengl32.glNormal3s(); }
void _glNormal3sv() { opengl32.glNormal3sv(); }
void _glNormalPointer() { opengl32.glNormalPointer(); }
void _glOrtho() { opengl32.glOrtho(); }
void _glPassThrough() { opengl32.glPassThrough(); }
void _glPixelMapfv() { opengl32.glPixelMapfv(); }
void _glPixelMapuiv() { opengl32.glPixelMapuiv(); }
void _glPixelMapusv() { opengl32.glPixelMapusv(); }
void _glPixelStoref() { opengl32.glPixelStoref(); }
void _glPixelStorei() { opengl32.glPixelStorei(); }
void _glPixelTransferf() { opengl32.glPixelTransferf(); }
void _glPixelTransferi() { opengl32.glPixelTransferi(); }
void _glPixelZoom() { opengl32.glPixelZoom(); }
void _glPointSize() { opengl32.glPointSize(); }
void _glPolygonMode() { opengl32.glPolygonMode(); }
void _glPolygonOffset() { opengl32.glPolygonOffset(); }
void _glPolygonStipple() { opengl32.glPolygonStipple(); }
void _glPopAttrib() { opengl32.glPopAttrib(); }
void _glPopClientAttrib() { opengl32.glPopClientAttrib(); }
void _glPopMatrix() { opengl32.glPopMatrix(); }
void _glPopName() { opengl32.glPopName(); }
void _glPrioritizeTextures() { opengl32.glPrioritizeTextures(); }
void _glPushAttrib() { opengl32.glPushAttrib(); }
void _glPushClientAttrib() { opengl32.glPushClientAttrib(); }
void _glPushMatrix() { opengl32.glPushMatrix(); }
void _glPushName() { opengl32.glPushName(); }
void _glRasterPos2d() { opengl32.glRasterPos2d(); }
void _glRasterPos2dv() { opengl32.glRasterPos2dv(); }
void _glRasterPos2f() { opengl32.glRasterPos2f(); }
void _glRasterPos2fv() { opengl32.glRasterPos2fv(); }
void _glRasterPos2i() { opengl32.glRasterPos2i(); }
void _glRasterPos2iv() { opengl32.glRasterPos2iv(); }
void _glRasterPos2s() { opengl32.glRasterPos2s(); }
void _glRasterPos2sv() { opengl32.glRasterPos2sv(); }
void _glRasterPos3d() { opengl32.glRasterPos3d(); }
void _glRasterPos3dv() { opengl32.glRasterPos3dv(); }
void _glRasterPos3f() { opengl32.glRasterPos3f(); }
void _glRasterPos3fv() { opengl32.glRasterPos3fv(); }
void _glRasterPos3i() { opengl32.glRasterPos3i(); }
void _glRasterPos3iv() { opengl32.glRasterPos3iv(); }
void _glRasterPos3s() { opengl32.glRasterPos3s(); }
void _glRasterPos3sv() { opengl32.glRasterPos3sv(); }
void _glRasterPos4d() { opengl32.glRasterPos4d(); }
void _glRasterPos4dv() { opengl32.glRasterPos4dv(); }
void _glRasterPos4f() { opengl32.glRasterPos4f(); }
void _glRasterPos4fv() { opengl32.glRasterPos4fv(); }
void _glRasterPos4i() { opengl32.glRasterPos4i(); }
void _glRasterPos4iv() { opengl32.glRasterPos4iv(); }
void _glRasterPos4s() { opengl32.glRasterPos4s(); }
void _glRasterPos4sv() { opengl32.glRasterPos4sv(); }
void _glReadBuffer() { opengl32.glReadBuffer(); }
void _glReadPixels() { opengl32.glReadPixels(); }
void _glRectd() { opengl32.glRectd(); }
void _glRectdv() { opengl32.glRectdv(); }
void _glRectf() { opengl32.glRectf(); }
void _glRectfv() { opengl32.glRectfv(); }
void _glRecti() { opengl32.glRecti(); }
void _glRectiv() { opengl32.glRectiv(); }
void _glRects() { opengl32.glRects(); }
void _glRectsv() { opengl32.glRectsv(); }
void _glRenderMode() { opengl32.glRenderMode(); }
void _glRotated() { opengl32.glRotated(); }
void _glRotatef() { opengl32.glRotatef(); }
void _glScaled() { opengl32.glScaled(); }
void _glScalef() { opengl32.glScalef(); }
void _glScissor() { opengl32.glScissor(); }
void _glSelectBuffer() { opengl32.glSelectBuffer(); }
void _glShadeModel() { opengl32.glShadeModel(); }
void _glStencilFunc() { opengl32.glStencilFunc(); }
void _glStencilMask() { opengl32.glStencilMask(); }
void _glStencilOp() { opengl32.glStencilOp(); }
void _glTexCoord1d() { opengl32.glTexCoord1d(); }
void _glTexCoord1dv() { opengl32.glTexCoord1dv(); }
void _glTexCoord1f() { opengl32.glTexCoord1f(); }
void _glTexCoord1fv() { opengl32.glTexCoord1fv(); }
void _glTexCoord1i() { opengl32.glTexCoord1i(); }
void _glTexCoord1iv() { opengl32.glTexCoord1iv(); }
void _glTexCoord1s() { opengl32.glTexCoord1s(); }
void _glTexCoord1sv() { opengl32.glTexCoord1sv(); }
void _glTexCoord2d() { opengl32.glTexCoord2d(); }
void _glTexCoord2dv() { opengl32.glTexCoord2dv(); }
void _glTexCoord2f() { opengl32.glTexCoord2f(); }
void _glTexCoord2fv() { opengl32.glTexCoord2fv(); }
void _glTexCoord2i() { opengl32.glTexCoord2i(); }
void _glTexCoord2iv() { opengl32.glTexCoord2iv(); }
void _glTexCoord2s() { opengl32.glTexCoord2s(); }
void _glTexCoord2sv() { opengl32.glTexCoord2sv(); }
void _glTexCoord3d() { opengl32.glTexCoord3d(); }
void _glTexCoord3dv() { opengl32.glTexCoord3dv(); }
void _glTexCoord3f() { opengl32.glTexCoord3f(); }
void _glTexCoord3fv() { opengl32.glTexCoord3fv(); }
void _glTexCoord3i() { opengl32.glTexCoord3i(); }
void _glTexCoord3iv() { opengl32.glTexCoord3iv(); }
void _glTexCoord3s() { opengl32.glTexCoord3s(); }
void _glTexCoord3sv() { opengl32.glTexCoord3sv(); }
void _glTexCoord4d() { opengl32.glTexCoord4d(); }
void _glTexCoord4dv() { opengl32.glTexCoord4dv(); }
void _glTexCoord4f() { opengl32.glTexCoord4f(); }
void _glTexCoord4fv() { opengl32.glTexCoord4fv(); }
void _glTexCoord4i() { opengl32.glTexCoord4i(); }
void _glTexCoord4iv() { opengl32.glTexCoord4iv(); }
void _glTexCoord4s() { opengl32.glTexCoord4s(); }
void _glTexCoord4sv() { opengl32.glTexCoord4sv(); }
void _glTexCoordPointer() { opengl32.glTexCoordPointer(); }
void _glTexEnvf() { opengl32.glTexEnvf(); }
void _glTexEnvfv() { opengl32.glTexEnvfv(); }
void _glTexEnvi() { opengl32.glTexEnvi(); }
void _glTexEnviv() { opengl32.glTexEnviv(); }
void _glTexGend() { opengl32.glTexGend(); }
void _glTexGendv() { opengl32.glTexGendv(); }
void _glTexGenf() { opengl32.glTexGenf(); }
void _glTexGenfv() { opengl32.glTexGenfv(); }
void _glTexGeni() { opengl32.glTexGeni(); }
void _glTexGeniv() { opengl32.glTexGeniv(); }
void _glTexImage1D() { opengl32.glTexImage1D(); }
void _glTexImage2D() { opengl32.glTexImage2D(); }
void _glTexParameterf() { opengl32.glTexParameterf(); }
void _glTexParameterfv() { opengl32.glTexParameterfv(); }
void _glTexParameteri() { opengl32.glTexParameteri(); }
void _glTexParameteriv() { opengl32.glTexParameteriv(); }
void _glTexSubImage1D() { opengl32.glTexSubImage1D(); }
void _glTexSubImage2D() { opengl32.glTexSubImage2D(); }
void _glTranslated() { opengl32.glTranslated(); }
void _glTranslatef() { opengl32.glTranslatef(); }
void _glVertex2d() { opengl32.glVertex2d(); }
void _glVertex2dv() { opengl32.glVertex2dv(); }
void _glVertex2f() { opengl32.glVertex2f(); }
void _glVertex2fv() { opengl32.glVertex2fv(); }
void _glVertex2i() { opengl32.glVertex2i(); }
void _glVertex2iv() { opengl32.glVertex2iv(); }
void _glVertex2s() { opengl32.glVertex2s(); }
void _glVertex2sv() { opengl32.glVertex2sv(); }
void _glVertex3d() { opengl32.glVertex3d(); }
void _glVertex3dv() { opengl32.glVertex3dv(); }
void _glVertex3f() { opengl32.glVertex3f(); }
void _glVertex3fv() { opengl32.glVertex3fv(); }
void _glVertex3i() { opengl32.glVertex3i(); }
void _glVertex3iv() { opengl32.glVertex3iv(); }
void _glVertex3s() { opengl32.glVertex3s(); }
void _glVertex3sv() { opengl32.glVertex3sv(); }
void _glVertex4d() { opengl32.glVertex4d(); }
void _glVertex4dv() { opengl32.glVertex4dv(); }
void _glVertex4f() { opengl32.glVertex4f(); }
void _glVertex4fv() { opengl32.glVertex4fv(); }
void _glVertex4i() { opengl32.glVertex4i(); }
void _glVertex4iv() { opengl32.glVertex4iv(); }
void _glVertex4s() { opengl32.glVertex4s(); }
void _glVertex4sv() { opengl32.glVertex4sv(); }
void _glVertexPointer() { opengl32.glVertexPointer(); }
void _glViewport() { opengl32.glViewport(); }
void _wglChoosePixelFormat() { opengl32.wglChoosePixelFormat(); }
void _wglCopyContext() { opengl32.wglCopyContext(); }
void _wglCreateContext() { opengl32.wglCreateContext(); }
void _wglCreateLayerContext() { opengl32.wglCreateLayerContext(); }
void _wglDeleteContext() { opengl32.wglDeleteContext(); }
void _wglDescribeLayerPlane() { opengl32.wglDescribeLayerPlane(); }
void _wglDescribePixelFormat() { opengl32.wglDescribePixelFormat(); }
void _wglGetCurrentContext() { opengl32.wglGetCurrentContext(); }
void _wglGetCurrentDC() { opengl32.wglGetCurrentDC(); }
void _wglGetDefaultProcAddress() { opengl32.wglGetDefaultProcAddress(); }
void _wglGetLayerPaletteEntries() { opengl32.wglGetLayerPaletteEntries(); }
void _wglGetPixelFormat() { opengl32.wglGetPixelFormat(); }
void _wglGetProcAddress() { opengl32.wglGetProcAddress(); }
void _wglMakeCurrent() { opengl32.wglMakeCurrent(); }
void _wglRealizeLayerPalette() { opengl32.wglRealizeLayerPalette(); }
void _wglSetLayerPaletteEntries() { opengl32.wglSetLayerPaletteEntries(); }
void _wglSetPixelFormat() { opengl32.wglSetPixelFormat(); }
void _wglShareLists() { opengl32.wglShareLists(); }
void _wglSwapBuffers() { opengl32.wglSwapBuffers(); }
void _wglSwapLayerBuffers() { opengl32.wglSwapLayerBuffers(); }
void _wglSwapMultipleBuffers() { opengl32.wglSwapMultipleBuffers(); }
void _wglUseFontBitmapsA() { opengl32.wglUseFontBitmapsA(); }
void _wglUseFontBitmapsW() { opengl32.wglUseFontBitmapsW(); }
void _wglUseFontOutlinesA() { opengl32.wglUseFontOutlinesA(); }
void _wglUseFontOutlinesW() { opengl32.wglUseFontOutlinesW(); }
