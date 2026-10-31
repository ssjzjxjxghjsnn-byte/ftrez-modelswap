using UnityEngine;

namespace FTRez.LobbyModel
{
    // Grow immediately; shrink only after a sustained saving. Retain the largest
    // demand during the grace period so walking does not churn render targets.
    internal sealed class CropSizing
    {
        float shrinkAt = -1;
        int peakWidth, peakHeight;
        public void Reset() { shrinkAt = -1; }
        public void Fit(ref int width, ref int height, int currentWidth, int currentHeight, float now)
        {
            if (width > currentWidth || height > currentHeight)
            {
                width = Mathf.Max(width, currentWidth); height = Mathf.Max(height, currentHeight);
                Reset(); return;
            }
            if (width > currentWidth - 128 && height > currentHeight - 128)
            { width = currentWidth; height = currentHeight; Reset(); return; }
            if (shrinkAt < 0)
            { shrinkAt = now + .75f; peakWidth = width; peakHeight = height; }
            peakWidth = Mathf.Max(peakWidth, width); peakHeight = Mathf.Max(peakHeight, height);
            if (now < shrinkAt) { width = currentWidth; height = currentHeight; return; }
            width = peakWidth <= currentWidth - 128 ? peakWidth : currentWidth;
            height = peakHeight <= currentHeight - 128 ? peakHeight : currentHeight;
            Reset();
        }
    }
}
