using System.Diagnostics;
using System.Text;

namespace FTRez.LobbyModel
{
    // CPU submission only, never a GPU timer or an FPS estimate. Fixed storage,
    // no per-frame strings; the caller flushes at most once every five seconds.
    internal static class RenderTiming
    {
        internal enum Part { Layers, Targets, OriginalBinding, OriginalState, Environment, ViewSetup, Stage0, StageLater, Foreground, Composite }
        static readonly double[] milliseconds = new double[10];
        static readonly int[] calls = new int[10];
        static readonly string[] names = System.Enum.GetNames(typeof(Part));
        static long compositePixels, compositeFull, foregroundPixels, foregroundFull;
        public static void Pixels(bool foreground, UnityEngine.Rect rect, int width, int height)
        {
            long pixels = (long)rect.width * (long)rect.height, full = (long)width * height;
            if (foreground) { foregroundPixels += pixels; foregroundFull += full; }
            else { compositePixels += pixels; compositeFull += full; }
        }
        public static long Start() { return Stopwatch.GetTimestamp(); }
        public static void End(Part part, long start)
        {
            int i = (int)part;
            milliseconds[i] += (Stopwatch.GetTimestamp() - start) * 1000.0 / Stopwatch.Frequency;
            ++calls[i];
        }
        public static string Flush()
        {
            var text = new StringBuilder();
            for (int i = 0; i < calls.Length; ++i)
            {
                if (calls[i] != 0) text.Append(' ').Append(names[i]).Append('=')
                    .Append((milliseconds[i] / calls[i]).ToString("F2")).Append("ms/").Append(calls[i]);
                milliseconds[i] = 0; calls[i] = 0;
            }
            if (compositeFull != 0) text.Append(" compositeArea=").Append((100.0 * compositePixels / compositeFull).ToString("F1")).Append('%');
            if (foregroundFull != 0) text.Append(" foregroundArea=").Append((100.0 * foregroundPixels / foregroundFull).ToString("F1")).Append('%');
            compositePixels = compositeFull = foregroundPixels = foregroundFull = 0;
            return text.ToString();
        }
    }
}
