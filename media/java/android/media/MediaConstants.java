/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.media;

// Code for AML only
class MediaConstants {
    // Bundle key for int
    static final String KEY_PID = "android.media.key.PID";

    // Bundle key for String
    static final String KEY_PACKAGE_NAME = "android.media.key.PACKAGE_NAME";

    // Bundle key for Parcelable
    static final String KEY_SESSION2_STUB = "android.media.key.SESSION2_STUB";
    static final String KEY_ALLOWED_COMMANDS = "android.media.key.ALLOWED_COMMANDS";

    private MediaConstants() {
    }
}
