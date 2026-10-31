using System;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace FTRez.LobbyModel
{
    internal static class GeometryChanges
    {
        // Owned for the native module's lifetime. Two aligned atomic counters;
        // no managed invocation is needed for ordinary engine mutations.
        public static IntPtr NativeState;
        static long sceneGeneration;
        static int pending;
        static bool installed;
        public static void Install()
        {
            if (installed) return;
            SceneManager.sceneLoaded += Loaded;
            SceneManager.sceneUnloaded += Unloaded;
            installed = true;
        }
        static void Loaded(Scene scene, LoadSceneMode mode) { ++sceneGeneration; }
        static void Unloaded(Scene scene) { ++sceneGeneration; }
        public static void Watch(AsyncOperation operation)
        {
            if (operation == null) throw new ArgumentNullException(nameof(operation));
            ++pending; ++sceneGeneration;
            operation.completed += Completed;
        }
        static void Completed(AsyncOperation operation) { --pending; ++sceneGeneration; }
        public static bool Read(out long generation, out long scenes)
        {
            scenes = sceneGeneration; generation = 0;
            if (NativeState == IntPtr.Zero) return false;
            generation = Marshal.ReadInt64(NativeState);
            System.Threading.Thread.MemoryBarrier();
            // Async integration can add geometry before its completion event.
            return pending == 0 && Marshal.ReadInt32(NativeState, 8) == 0;
        }
    }
}
