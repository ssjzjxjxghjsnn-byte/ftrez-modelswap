using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

public static class ValidateGLTF
{
    static Texture2D Texture(byte r, byte g, byte b, byte a, bool linear)
    {
        var image = new Texture2D(2, 2, TextureFormat.RGBA32, true, linear);
        image.SetPixels32(new[] { new Color32(r,g,b,a), new Color32(r,g,b,a), new Color32(r,g,b,a), new Color32(r,g,b,a) });
        image.Apply(true, true);
        return image;
    }
    static Color Pixel(RenderTexture target)
    {
        var old = RenderTexture.active;
        RenderTexture.active = target;
        var read = new Texture2D(target.width, target.height, TextureFormat.RGBAFloat, false, true);
        read.ReadPixels(new Rect(0, 0, target.width, target.height), 0, 0);
        read.Apply();
        var color = read.GetPixel(target.width/2, target.height/2);
        UnityEngine.Object.DestroyImmediate(read);
        RenderTexture.active = old;
        return color;
    }
    static Color Capture(Camera camera)
    {
        RenderTexture.active = null;
        for (int frame = 0; frame < 4; ++frame) camera.Render();
        return Pixel(camera.targetTexture);
    }
    static void Near(float actual, float expected, string label, float tolerance=.002f)
    {
        if (float.IsNaN(actual) || float.IsInfinity(actual) || Mathf.Abs(actual-expected)>tolerance) throw new Exception(label+": "+actual+" expected "+expected);
    }
    [Serializable] class Result
    {
        public string colorSpace, graphics;
        public float srgbSample, linearSample, baseFactorResult, blendResult, emissionResult, metallicRoughnessError, normalDifference;
        public bool passed;
    }
    public static void Run()
    {
        BuildFTRezToon.Configure();
        EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        RenderSettings.fog = false;
        RenderSettings.ambientMode = AmbientMode.Flat;
        RenderSettings.ambientLight = Color.black;
        RenderSettings.reflectionIntensity = 0;
        var shader = AssetDatabase.LoadAssetAtPath<Shader>("Assets/FTRezGLTF/GLTF.shader");
        if (!shader || ShaderUtil.ShaderHasError(shader)) throw new Exception("GLTF shader unavailable");
        if (QualitySettings.activeColorSpace != ColorSpace.Linear) throw new Exception("Validation must run in Linear");
        var target = new RenderTexture(64,64,24,RenderTextureFormat.ARGBFloat,RenderTextureReadWrite.Linear);
        target.Create();
        var camera = new GameObject("Validation camera").AddComponent<Camera>();
        camera.transform.position = new Vector3(0,0,-2);
        camera.orthographic = true; camera.orthographicSize = .6f;
        camera.clearFlags = CameraClearFlags.SolidColor; camera.backgroundColor = Color.black;
        camera.allowHDR = true; camera.allowMSAA = false;
        camera.nearClipPlane = .1f; camera.farClipPlane = 10;
        camera.targetTexture = target;
        var data = camera.GetUniversalAdditionalCameraData();
        data.renderPostProcessing = false; data.antialiasing = AntialiasingMode.None;
        var quad = GameObject.CreatePrimitive(PrimitiveType.Quad);
        var material = new Material(shader);
        quad.GetComponent<Renderer>().sharedMaterial = material;
        var srgb = Texture(128,128,128,102,false);
        var linear = Texture(128,128,128,102,true);
        Graphics.Blit(srgb,target); var srgbSample = Pixel(target).r;
        Graphics.Blit(linear,target); var linearSample = Pixel(target).r;
        Near(srgbSample,Mathf.GammaToLinearSpace(128/255f),"sRGB sample");
        Near(linearSample,128/255f,"Linear data sample");
        material.SetTexture("_BaseMap",srgb);
        material.SetVector("_BaseColor",new Vector4(.25f,.5f,.75f,1));
        material.SetFloat("_Unlit",1);
        var opaque = Capture(camera);
        Near(opaque.r,srgbSample*.25f,"Linear base factor"); Near(opaque.a,1,"Opaque alpha");
        material.SetFloat("_AlphaMode",1); material.SetFloat("_Cutoff",.5f);
        Near(Capture(camera).r,0,"Masked fragment");
        material.SetFloat("_Cutoff",.2f); Near(Capture(camera).r,opaque.r,"Accepted mask");
        material.SetFloat("_AlphaMode",2); material.SetFloat("_SrcBlend",5); material.SetFloat("_DstBlend",10);
        material.SetFloat("_ZWrite",0); material.renderQueue=3000;
        var blended=Capture(camera);
        Near(blended.r,opaque.r*(102/255f),"Straight alpha blend");
        material.SetFloat("_AlphaMode",0); material.SetFloat("_SrcBlend",1); material.SetFloat("_DstBlend",0);
        material.SetFloat("_ZWrite",1); material.renderQueue=2000;
        material.SetFloat("_Unlit",0); material.SetTexture("_EmissionMap",srgb);
        material.SetVector("_EmissionColor",new Vector4(.2f,.3f,.4f,1));
        var emissive=Capture(camera);
        Near(emissive.r,srgbSample*.2f,"Linear emissive factor",.003f);
        material.SetVector("_EmissionColor",Vector4.zero);
        var light = new GameObject("PBR main light").AddComponent<Light>();
        light.type=LightType.Directional; light.intensity=1;
        light.transform.rotation=Quaternion.Euler(20,30,0);
        var mr=Texture(231,77,204,255,true);
        material.SetTexture("_MetallicRoughnessMap",mr);
        material.SetFloat("_Metallic",1); material.SetFloat("_Roughness",1);
        var sampled=Capture(camera);
        material.SetTexture("_MetallicRoughnessMap",Texture2D.whiteTexture);
        material.SetFloat("_Metallic",204/255f); material.SetFloat("_Roughness",77/255f);
        var factored=Capture(camera);
        Near(sampled.r,factored.r,"Metal B and roughness G",.003f);
        material.SetFloat("_Metallic",0); material.SetFloat("_Roughness",1);
        var flat=Capture(camera);
        material.SetTexture("_NormalMap",Texture(230,128,204,255,true)); material.SetFloat("_HasNormalMap",1);
        var tilted=Capture(camera);
        if(Mathf.Abs(flat.r-tilted.r)<.001f) throw new Exception("Normal map did not affect lighting");
        var result=new Result { colorSpace=QualitySettings.activeColorSpace.ToString(), graphics=SystemInfo.graphicsDeviceType.ToString(),
            srgbSample=srgbSample,linearSample=linearSample,baseFactorResult=opaque.r,blendResult=blended.r,
            emissionResult=emissive.r,metallicRoughnessError=Mathf.Abs(sampled.r-factored.r),normalDifference=Mathf.Abs(flat.r-tilted.r),passed=true };
        Directory.CreateDirectory("GLTFValidation");
        File.WriteAllText("GLTFValidation/report.json",JsonUtility.ToJson(result,true));
        Debug.Log("GLTF Linear, alpha, emission, metallic/roughness and normal validation passed");
    }
}
