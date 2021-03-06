/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class com_derecho_objectstore_ObjectStoreService */

#ifndef _Included_com_derecho_objectstore_ObjectStoreService
#define _Included_com_derecho_objectstore_ObjectStoreService
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     com_derecho_objectstore_ObjectStoreService
 * Method:    initialize
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_derecho_objectstore_ObjectStoreService_initialize
  (JNIEnv *, jobject, jstring);

/*
 * Class:     com_derecho_objectstore_ObjectStoreService
 * Method:    put
 * Signature: (JLjava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_derecho_objectstore_ObjectStoreService_put
  (JNIEnv *, jobject, jlong, jstring);

/*
 * Class:     com_derecho_objectstore_ObjectStoreService
 * Method:    remove
 * Signature: (J)Z
 */
JNIEXPORT jboolean JNICALL Java_com_derecho_objectstore_ObjectStoreService_remove
  (JNIEnv *, jobject, jlong);

/*
 * Class:     com_derecho_objectstore_ObjectStoreService
 * Method:    get
 * Signature: (J)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_com_derecho_objectstore_ObjectStoreService_get
  (JNIEnv *, jobject, jlong);

/*
 * Class:     com_derecho_objectstore_ObjectStoreService
 * Method:    leave
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_com_derecho_objectstore_ObjectStoreService_leave
  (JNIEnv *, jobject);

#ifdef __cplusplus
}
#endif
#endif
