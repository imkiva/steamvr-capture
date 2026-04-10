using System.IO;
using UnityEditor;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace SteamVRCapture.UnityExample.Editor
{
    [ScriptedImporter(4, "svrcap")]
    public sealed class SteamVrCaptureSessionImporter : ScriptedImporter
    {
        public override void OnImportAsset(AssetImportContext ctx)
        {
            SteamVrCaptureSessionAsset asset = ScriptableObject.CreateInstance<SteamVrCaptureSessionAsset>();
            asset.name = Path.GetFileNameWithoutExtension(ctx.assetPath);

            try
            {
                string projectRoot = Path.GetDirectoryName(Application.dataPath) ?? string.Empty;
                string absolutePath = Path.GetFullPath(Path.Combine(projectRoot, ctx.assetPath));
                SteamVrCaptureSessionParser.ParseResult parsed = SteamVrCaptureSessionParser.ParseFile(absolutePath);
                asset.SetImportedData(
                    parsed.formatVersion,
                    parsed.posesAreDriverSpace,
                    parsed.durationNs,
                    ctx.assetPath,
                    parsed.trackingSpace,
                    parsed.tracks);
            }
            catch (System.Exception exception)
            {
                asset.SetImportError(ctx.assetPath, exception.Message);
            }

            ctx.AddObjectToAsset("session", asset);
            ctx.SetMainObject(asset);
        }
    }
}
