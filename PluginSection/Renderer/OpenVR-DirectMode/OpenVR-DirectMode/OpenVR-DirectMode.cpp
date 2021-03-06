/********************************************************************
Vireio Perception: Open-Source Stereoscopic 3D Driver
Copyright (C) 2012 Andres Hernandez

OpenVR DirectMode - Open Virtual Reality Direct Mode Rendering Node
Copyright (C) 2016 Denis Reischl

File <OpenVR-DirectMode.cpp> and
Class <OpenVR-DirectMode> :
Copyright (C) 2016 Denis Reischl

The stub class <AQU_Nodus> is the only public class from the Aquilinus
repository and permitted to be used for open source plugins of any kind.
Read the Aquilinus documentation for further information.

Vireio Perception Version History:
v1.0.0 2012 by Andres Hernandez
v1.0.X 2013 by John Hicks, Neil Schneider
v1.1.x 2013 by Primary Coding Author: Chris Drain
Team Support: John Hicks, Phil Larkson, Neil Schneider
v2.0.x 2013 by Denis Reischl, Neil Schneider, Joshua Brown
v2.0.4 to v3.0.x 2014-2015 by Grant Bagwell, Simon Brown and Neil Schneider
v4.0.x 2015 by Denis Reischl, Grant Bagwell, Simon Brown, Samuel Austin
and Neil Schneider

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
********************************************************************/

#include"OpenVR-DirectMode.h"

#define DEBUG_UINT(a) { wchar_t buf[128]; wsprintf(buf, L"- %u", a); OutputDebugString(buf); }
#define DEBUG_HEX(a) { wchar_t buf[128]; wsprintf(buf, L"- %x", a); OutputDebugString(buf); }

#define INTERFACE_IDIRECT3DDEVICE9           8
#define INTERFACE_IDIRECT3DSWAPCHAIN9       15
#define INTERFACE_IDXGISWAPCHAIN            29

#define	METHOD_IDIRECT3DDEVICE9_PRESENT     17
#define	METHOD_IDIRECT3DDEVICE9_ENDSCENE    42
#define	METHOD_IDIRECT3DSWAPCHAIN9_PRESENT   3
#define METHOD_IDXGISWAPCHAIN_PRESENT        8

#pragma region OpenVR-Tracker static fields
bool OpenVR_DirectMode::m_bInit;
vr::IVRSystem **OpenVR_DirectMode::m_ppHMD;
ID3D11Texture2D *OpenVR_DirectMode::m_pcTex11Shared[2];
HANDLE OpenVR_DirectMode::m_pThread;
float OpenVR_DirectMode::m_fHorizontalOffsetCorrectionLeft;
float OpenVR_DirectMode::m_fHorizontalOffsetCorrectionRight;
float OpenVR_DirectMode::m_fHorizontalRatioCorrectionLeft;
float OpenVR_DirectMode::m_fHorizontalRatioCorrectionRight;
bool OpenVR_DirectMode::m_bForceInterleavedReprojection;
DWORD OpenVR_DirectMode::m_unSleepTime;
#pragma endregion

/**
* Matrix translation helper.
***/
inline vr::HmdMatrix34_t Translate_3x4(vr::HmdVector3_t sV)
{
	vr::HmdMatrix34_t sRet;

	sRet.m[0][0] = 1.0f; sRet.m[1][0] = 0.0f; sRet.m[2][0] = 0.0f;
	sRet.m[0][1] = 0.0f; sRet.m[1][1] = 1.0f; sRet.m[2][1] = 0.0f;
	sRet.m[0][2] = 0.0f; sRet.m[1][2] = 0.0f; sRet.m[2][2] = 1.0f;
	sRet.m[0][3] = sV.v[0]; sRet.m[1][3] = sV.v[1]; sRet.m[2][3] = sV.v[2];

	return sRet;
}

/**
* Little helper.
***/
HRESULT CreateCopyTexture(ID3D11Device* pcDevice, ID3D11Device* pcDeviceTemporary, ID3D11ShaderResourceView* pcSRV, ID3D11Texture2D** ppcDest, ID3D11Texture2D**ppcDestDraw, ID3D11RenderTargetView** ppcDestDrawRTV, ID3D11Texture2D** ppcDestShared)
{
	ID3D11Resource* pcResource = nullptr;
	pcSRV->GetResource(&pcResource);
	if (pcResource)
	{
		// get the description and create the copy texture
		D3D11_TEXTURE2D_DESC sDesc;
		((ID3D11Texture2D*)pcResource)->GetDesc(&sDesc);
		pcResource->Release();

		sDesc.ArraySize = 1;
		sDesc.CPUAccessFlags = 0;
		sDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sDesc.MipLevels = 1;
		sDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		sDesc.SampleDesc.Count = 1;
		sDesc.SampleDesc.Quality = 0;
		sDesc.Usage = D3D11_USAGE_DEFAULT;
		sDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		// create the copy texture
		if (FAILED(pcDevice->CreateTexture2D(&sDesc, NULL, ppcDest)))
		{
			OutputDebugString(L"OpenVR : Failed to create copy texture !");
			return E_FAIL;
		}

		// create only if needed
		if ((ppcDestDraw) && (ppcDestDrawRTV))
		{
			// create the drawing texture and view
			sDesc.MiscFlags = 0;
			sDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (FAILED(pcDevice->CreateTexture2D(&sDesc, NULL, ppcDestDraw)))
			{
				(*ppcDest)->Release(); (*ppcDest) = nullptr;
				OutputDebugString(L"OpenVR : Failed to create overlay texture !");
				return E_FAIL;
			}
			pcDevice->CreateRenderTargetView((ID3D11Resource*)*ppcDestDraw, NULL, ppcDestDrawRTV);
		}

		// get shared handle
		IDXGIResource* pcDXGIResource(NULL);
		(*ppcDest)->QueryInterface(__uuidof(IDXGIResource), (void**)&pcDXGIResource);
		HANDLE sharedHandle;
		if (pcDXGIResource)
		{
			pcDXGIResource->GetSharedHandle(&sharedHandle);
			pcDXGIResource->Release();
		}
		else OutputDebugString(L"Failed to query IDXGIResource.");

		// open the shared handle with the TEMPORARY (!) device
		ID3D11Resource* pcResourceShared;
		pcDeviceTemporary->OpenSharedResource(sharedHandle, __uuidof(ID3D11Resource), (void**)(&pcResourceShared));
		if (pcResourceShared)
		{
			pcResourceShared->QueryInterface(__uuidof(ID3D11Texture2D), (void**)ppcDestShared);
			pcResourceShared->Release();
		}
		else OutputDebugString(L"Could not open shared resource.");
	}
	return S_OK;
}

/**
* Ini file helper.
***/
DWORD GetIniFileSetting(DWORD unDefault, LPCSTR szAppName, LPCSTR szKeyName, LPCSTR szFileName, bool bFileExists)
{
	DWORD unRet = 0;
	char szBuffer[128];

	if (bFileExists)
	{
		// fov
		std::stringstream sz;
		sz << unDefault;
		GetPrivateProfileStringA(szAppName, szKeyName, sz.str().c_str(), szBuffer, 128, szFileName);
		sz = std::stringstream(szBuffer);
		sz >> unRet;
	}
	else
	{
		// fov
		std::stringstream sz;
		sz << unDefault;
		WritePrivateProfileStringA(szAppName, szKeyName, sz.str().c_str(), szFileName);
		unRet = unDefault;
	}

	return unRet;
}

/**
* Ini file helper.
***/
float GetIniFileSetting(float fDefault, LPCSTR szAppName, LPCSTR szKeyName, LPCSTR szFileName, bool bFileExists)
{
	float fRet = 0;
	char szBuffer[128];

	if (bFileExists)
	{
		// fov
		std::stringstream sz;
		sz << fDefault;
		GetPrivateProfileStringA(szAppName, szKeyName, sz.str().c_str(), szBuffer, 128, szFileName);
		sz = std::stringstream(szBuffer);
		sz >> fRet;
	}
	else
	{
		// fov
		std::stringstream sz;
		sz << fDefault;
		WritePrivateProfileStringA(szAppName, szKeyName, sz.str().c_str(), szFileName);
		fRet = fDefault;
	}

	return fRet;
}


/**
* Constructor.
***/
OpenVR_DirectMode::OpenVR_DirectMode() : AQU_Nodus(),
m_pcDeviceTemporary(nullptr),
m_pcContextTemporary(nullptr),
m_ulOverlayHandle(0),
m_ulOverlayThumbnailHandle(0),
m_bHotkeySwitch(false),
m_pbZoomOut(nullptr)
{
	m_ppcTexView11[0] = nullptr;
	m_ppcTexView11[1] = nullptr;
	m_pcTex11Copy[0] = nullptr;
	m_pcTex11Copy[1] = nullptr;
	m_pcTex11CopyHUD = nullptr;
	m_pcTex11Draw[0] = nullptr;
	m_pcTex11Draw[1] = nullptr;
	m_pcTex11DrawRTV[0] = nullptr;
	m_pcTex11DrawRTV[1] = nullptr;
	m_pcTex11Shared[0] = nullptr;
	m_pcTex11Shared[1] = nullptr;
	m_pcTex11SharedHUD = nullptr;

	m_pcVertexShader11 = nullptr;
	m_pcPixelShader11 = nullptr;
	m_pcVertexLayout11 = nullptr;
	m_pcVertexBuffer11 = nullptr;
	m_pcConstantBufferDirect11 = nullptr;
	m_pcSamplerState = nullptr;

	// static fields
	m_bInit = false;
	m_ppHMD = nullptr;
	m_pThread = nullptr;
	m_fHorizontalRatioCorrectionLeft = 0.0f;
	m_fHorizontalRatioCorrectionRight = 0.0f;
	m_fHorizontalOffsetCorrectionLeft = 0.0f;
	m_fHorizontalOffsetCorrectionRight = 0.0f;

	//----------------------

	// set default ini settings
	m_fAspectRatio = 1920.0f / 1080.0f;
	m_bForceInterleavedReprojection = true;
	m_unSleepTime = 20;

	// set default overlay properties
	m_sOverlayPropertiesHud.eTransform = OverlayTransformType::Absolute;
	switch (m_sOverlayPropertiesHud.eTransform)
	{
		case OpenVR_DirectMode::Absolute:
			m_sOverlayPropertiesHud.eOrigin = vr::ETrackingUniverseOrigin::TrackingUniverseStanding;
			break;
		case OpenVR_DirectMode::TrackedDeviceRelative:
			m_sOverlayPropertiesHud.nDeviceIndex = 0;
			break;
		case OpenVR_DirectMode::TrackedDeviceComponent:
			break;
	}
	m_sOverlayPropertiesHud.sVectorTranslation.v[0] = 0.0f;
	m_sOverlayPropertiesHud.sVectorTranslation.v[1] = 2.0f;
	m_sOverlayPropertiesHud.sVectorTranslation.v[2] = -2.0f;
	m_sOverlayPropertiesHud.sColor.a = 1.0f;
	m_sOverlayPropertiesHud.sColor.r = 1.0f;
	m_sOverlayPropertiesHud.sColor.g = 1.0f;
	m_sOverlayPropertiesHud.sColor.b = 1.0f;
	m_sOverlayPropertiesHud.fWidth = 3.0f;

	m_sOverlayPropertiesDashboard.sColor.a = 1.0f;
	m_sOverlayPropertiesDashboard.sColor.r = 1.0f;
	m_sOverlayPropertiesDashboard.sColor.g = 1.0f;
	m_sOverlayPropertiesDashboard.sColor.b = 1.0f;
	m_sOverlayPropertiesDashboard.fWidth = 3.0f;

	// locate or create the INI file
	char szFilePathINI[1024];
	GetCurrentDirectoryA(1024, szFilePathINI);
	strcat_s(szFilePathINI, "\\VireioPerception_OpenVR.ini");
	bool bFileExists = false; 
	if (PathFileExistsA(szFilePathINI)) bFileExists = true;
	
	// get all ini settings
	m_fAspectRatio = GetIniFileSetting(m_fAspectRatio, "OpenVR", "fAspectRatio", szFilePathINI, bFileExists);
	if (GetIniFileSetting((DWORD)m_bForceInterleavedReprojection, "OpenVR", "bForceInterleavedReprojection", szFilePathINI, bFileExists)) m_bForceInterleavedReprojection = true; else m_bForceInterleavedReprojection = false;
	m_unSleepTime = GetIniFileSetting(m_unSleepTime, "OpenVR", "unSleepTime", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.eTransform = (OverlayTransformType)GetIniFileSetting((DWORD)m_sOverlayPropertiesHud.eTransform, "OpenVR", "sOverlayPropertiesHud.eTransform", szFilePathINI, bFileExists);
	switch (m_sOverlayPropertiesHud.eTransform)
	{
		case OpenVR_DirectMode::Absolute:
			m_sOverlayPropertiesHud.eOrigin = (vr::ETrackingUniverseOrigin)GetIniFileSetting((DWORD)m_sOverlayPropertiesHud.eOrigin, "OpenVR", "sOverlayPropertiesHud.eOrigin", szFilePathINI, bFileExists);
			GetIniFileSetting((DWORD)0, "OpenVR", "sOverlayPropertiesHud.nDeviceIndex", szFilePathINI, bFileExists);
			break;
		case OpenVR_DirectMode::TrackedDeviceRelative:
			GetIniFileSetting((DWORD)0, "OpenVR", "sOverlayPropertiesHud.eOrigin", szFilePathINI, bFileExists);
			m_sOverlayPropertiesHud.nDeviceIndex = (vr::TrackedDeviceIndex_t)GetIniFileSetting((DWORD)m_sOverlayPropertiesHud.nDeviceIndex, "OpenVR", "sOverlayPropertiesHud.nDeviceIndex", szFilePathINI, bFileExists);
			break;
		case OpenVR_DirectMode::TrackedDeviceComponent:
			break;
	}
	m_sOverlayPropertiesHud.sVectorTranslation.v[0] = GetIniFileSetting(m_sOverlayPropertiesHud.sVectorTranslation.v[0], "OpenVR", "sOverlayPropertiesHud.sVectorTranslation.v[0]", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sVectorTranslation.v[1] = GetIniFileSetting(m_sOverlayPropertiesHud.sVectorTranslation.v[1], "OpenVR", "sOverlayPropertiesHud.sVectorTranslation.v[1]", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sVectorTranslation.v[2] = GetIniFileSetting(m_sOverlayPropertiesHud.sVectorTranslation.v[2], "OpenVR", "sOverlayPropertiesHud.sVectorTranslation.v[2]", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sColor.a = GetIniFileSetting(m_sOverlayPropertiesHud.sColor.a, "OpenVR", "sOverlayPropertiesHud.sColor.a", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sColor.r = GetIniFileSetting(m_sOverlayPropertiesHud.sColor.r, "OpenVR", "sOverlayPropertiesHud.sColor.r", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sColor.g = GetIniFileSetting(m_sOverlayPropertiesHud.sColor.g, "OpenVR", "sOverlayPropertiesHud.sColor.g", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.sColor.b = GetIniFileSetting(m_sOverlayPropertiesHud.sColor.b, "OpenVR", "sOverlayPropertiesHud.sColor.b", szFilePathINI, bFileExists);
	m_sOverlayPropertiesHud.fWidth = GetIniFileSetting(m_sOverlayPropertiesHud.fWidth, "OpenVR", "sOverlayPropertiesHud.fWidth", szFilePathINI, bFileExists);

	m_sOverlayPropertiesDashboard.sColor.a = GetIniFileSetting(m_sOverlayPropertiesDashboard.sColor.a, "OpenVR", "sOverlayPropertiesDashboard.sColor.a", szFilePathINI, bFileExists);
	m_sOverlayPropertiesDashboard.sColor.r = GetIniFileSetting(m_sOverlayPropertiesDashboard.sColor.r, "OpenVR", "sOverlayPropertiesDashboard.sColor.r", szFilePathINI, bFileExists);
	m_sOverlayPropertiesDashboard.sColor.g = GetIniFileSetting(m_sOverlayPropertiesDashboard.sColor.g, "OpenVR", "sOverlayPropertiesDashboard.sColor.g", szFilePathINI, bFileExists);
	m_sOverlayPropertiesDashboard.sColor.b = GetIniFileSetting(m_sOverlayPropertiesDashboard.sColor.b, "OpenVR", "sOverlayPropertiesDashboard.sColor.b", szFilePathINI, bFileExists);
	m_sOverlayPropertiesDashboard.fWidth = GetIniFileSetting(m_sOverlayPropertiesDashboard.fWidth, "OpenVR", "sOverlayPropertiesDashboard.fWidth", szFilePathINI, bFileExists);
}

/**
* Destructor.
***/
OpenVR_DirectMode::~OpenVR_DirectMode()
{
	if (m_pcContextTemporary) m_pcContextTemporary->Release();
	if (m_pcDeviceTemporary) m_pcDeviceTemporary->Release();

	if (m_pcTex11Copy[0]) m_pcTex11Copy[0]->Release();
	if (m_pcTex11Copy[1]) m_pcTex11Copy[1]->Release();
	if (m_pcTex11Draw[0]) m_pcTex11Draw[0]->Release();
	if (m_pcTex11Draw[1]) m_pcTex11Draw[1]->Release();
	if (m_pcTex11DrawRTV[0]) m_pcTex11DrawRTV[0]->Release();
	if (m_pcTex11DrawRTV[1]) m_pcTex11DrawRTV[1]->Release();
	if (m_pcTex11Shared[0]) m_pcTex11Shared[0]->Release();
	if (m_pcTex11Shared[1]) m_pcTex11Shared[1]->Release();

	TerminateThread(m_pThread, S_OK);
}

/**
* Return the name of the  OpenVR DirectMode node.
***/
const char* OpenVR_DirectMode::GetNodeType()
{
	return "OpenVR DirectMode";
}

/**
* Returns a global unique identifier for the OpenVR DirectMode node.
***/
UINT OpenVR_DirectMode::GetNodeTypeId()
{
#define DEVELOPER_IDENTIFIER 2006
#define MY_PLUGIN_IDENTIFIER 321
	return ((DEVELOPER_IDENTIFIER << 16) + MY_PLUGIN_IDENTIFIER);
}

/**
* Returns the name of the category for the OpenVR DirectMode node.
***/
LPWSTR OpenVR_DirectMode::GetCategory()
{
	return L"Renderer";
}

/**
* Returns a logo to be used for the OpenVR DirectMode node.
***/
HBITMAP OpenVR_DirectMode::GetLogo()
{
	HMODULE hModule = GetModuleHandle(L"OpenVR-DirectMode.dll");
	HBITMAP hBitmap = LoadBitmap(hModule, MAKEINTRESOURCE(IMG_LOGO01));
	return hBitmap;
}

/**
* Returns the updated control for the OpenVR DirectMode node.
* Allways return >nullptr< if there is no update for the control !!
***/
HBITMAP OpenVR_DirectMode::GetControl()
{
	return nullptr;
}

/**
* Provides the name of the requested decommander.
***/
LPWSTR OpenVR_DirectMode::GetDecommanderName(DWORD dwDecommanderIndex)
{
	switch ((OpenVR_Decommanders)dwDecommanderIndex)
	{
		case OpenVR_Decommanders::LeftTexture11:
			return L"Left Texture DX11";
		case OpenVR_Decommanders::RightTexture11:
			return L"Right Texture DX11";
		case OpenVR_Decommanders::IVRSystem:
			return L"IVRSystem";
		case OpenVR_Decommanders::ZoomOut:
			return L"Zoom Out";
		case HUDTexture11:
			return L"HUD Texture DX11";
		case HUDTexture10:
			break;
		case HUDTexture9:
			break;
	}

	return L"x";
}

/**
* Returns the plug type for the requested decommander.
***/
DWORD OpenVR_DirectMode::GetDecommanderType(DWORD dwDecommanderIndex)
{
	switch ((OpenVR_Decommanders)dwDecommanderIndex)
	{
		case OpenVR_Decommanders::LeftTexture11:
			return NOD_Plugtype::AQU_PNT_ID3D11SHADERRESOURCEVIEW;
		case OpenVR_Decommanders::RightTexture11:
			return NOD_Plugtype::AQU_PNT_ID3D11SHADERRESOURCEVIEW;
		case OpenVR_Decommanders::IVRSystem:
			return NOD_Plugtype::AQU_HANDLE;
		case OpenVR_Decommanders::ZoomOut:
			return NOD_Plugtype::AQU_BOOL;
		case HUDTexture11:
			return NOD_Plugtype::AQU_PNT_ID3D11SHADERRESOURCEVIEW;
		case HUDTexture10:
			break;
		case HUDTexture9:
			break;
	}

	return 0;
}

/**
* Sets the input pointer for the requested decommander.
***/
void OpenVR_DirectMode::SetInputPointer(DWORD dwDecommanderIndex, void* pData)
{
	switch ((OpenVR_Decommanders)dwDecommanderIndex)
	{
		case OpenVR_Decommanders::LeftTexture11:
			m_ppcTexView11[0] = (ID3D11ShaderResourceView**)pData;
			break;
		case OpenVR_Decommanders::RightTexture11:
			m_ppcTexView11[1] = (ID3D11ShaderResourceView**)pData;
			break;
		case OpenVR_Decommanders::IVRSystem:
			m_ppHMD = (vr::IVRSystem**)pData;
			break;
		case OpenVR_Decommanders::ZoomOut:
			m_pbZoomOut = (BOOL*)pData;
			break;
		case HUDTexture11:
			m_ppcTexViewHud11 = (ID3D11ShaderResourceView**)pData;
			break;
		case HUDTexture10:
			break;
		case HUDTexture9:
			break;
	}
}

/**
* DirectMode supports any calls.
***/
bool OpenVR_DirectMode::SupportsD3DMethod(int nD3DVersion, int nD3DInterface, int nD3DMethod)
{
	return true;
}

/**
* Handle OpenVR tracking.
***/
void* OpenVR_DirectMode::Provoke(void* pThis, int eD3D, int eD3DInterface, int eD3DMethod, DWORD dwNumberConnected, int& nProvokerIndex)
{
	// submit thread id
	static DWORD unThreadId = 0;

	// aspect ratio static fields... optimized now for 1920x1080
	static float fAspectRatio = m_fAspectRatio;
	static float bAspectRatio = false;

	if (eD3DInterface != INTERFACE_IDXGISWAPCHAIN) return nullptr;
	if (eD3DMethod != METHOD_IDXGISWAPCHAIN_PRESENT) return nullptr;
	/*if (!m_bHotkeySwitch)
	{
	if (GetAsyncKeyState(VK_F11))
	{
	m_bHotkeySwitch = true;
	}
	return nullptr;
	}*/

#pragma region aspect ratio / separation offset
	// FOV (HTC Vive):
	// LENS-CAMERA DISTANCE  0MM  5MM 10MM 15MM 20MM 25MM
	// LEFT HORIZONTAL  FOV  44�  46�  46�  46�  44�  39�
	// RIGHT HORIZONTAL FOV  48�  53�  54�  47�  41�  37�
	// TOTAL HORIZONTAL FOV  92�  99� 100�  93�  85�  76�
	// VERTICAL         FOV 102� 112� 113�  99�  88�  77�
	// Unlike the Oculus Rift DK2, HTC Vive DK1 / Pre reach their maximal fields 
	// of view at some distance from the lens, specifically at 8mm eye relief, 
	// which is practically achievable.Also unlike Rift DK2, the Vive DK1 / Pre�s 
	// view frusta are skewed outwards, sacrificing stereo overlap for increased 
	// binocular FoV.At the ideal eye relief of 8mm, and again assuming that the 
	// frusta are mirror images, total binocular FoV is 110� x 113�(not included 
	// in above table).To reiterate, measuring FoV offset accurately is rather hard, 
	// and the resulting binocular FoV estimates, unlike monocular FoV measurements, are to be taken with a grain of salt.
	// ( Oliver Kreylos FOV measurements )

	// Vireio Perception FOV recommandation: (based on Kreylos data)
	// Resolution (1920x1080)
	// HTC Vive : V 112� H 139�

	// calculate aspect ratio + offset correction..for OpenVR we need to set UV coords as aspect ratio
	if (!bAspectRatio)
	{
		// compute left eye 
		float fLeft, fRight, fTop, fBottom;
		(*m_ppHMD)->GetProjectionRaw(vr::Eye_Left, &fLeft, &fRight, &fTop, &fBottom);
		float fHorizontal = fRight - fLeft;
		float fVertical = fBottom - fTop;
		float fAspectHMD = fHorizontal / fVertical;
		m_fHorizontalRatioCorrectionLeft = (1.0f - (fAspectHMD / fAspectRatio)) / 2.0f;
		m_fHorizontalOffsetCorrectionLeft = abs(fRight) - abs(fLeft);
		m_fHorizontalOffsetCorrectionLeft /= fHorizontal;
		m_fHorizontalOffsetCorrectionLeft *= fAspectHMD / fAspectRatio;
		m_fHorizontalOffsetCorrectionLeft *= 0.5f;

		// compute right eye 
		(*m_ppHMD)->GetProjectionRaw(vr::Eye_Right, &fLeft, &fRight, &fTop, &fBottom);
		fHorizontal = fRight - fLeft;
		fVertical = fBottom - fTop;
		fAspectHMD = fHorizontal / fVertical;
		m_fHorizontalRatioCorrectionRight = (1.0f - (fAspectHMD / fAspectRatio)) / 2.0f;
		m_fHorizontalOffsetCorrectionRight = abs(fRight) - abs(fLeft);
		m_fHorizontalOffsetCorrectionRight /= fHorizontal;
		m_fHorizontalOffsetCorrectionRight *= fAspectHMD / fAspectRatio;
		m_fHorizontalOffsetCorrectionRight *= 0.5f;

		bAspectRatio = true;
	}
#pragma endregion

	// create thread
	if (!unThreadId)
		m_pThread = CreateThread(NULL, 0, SubmitFramesConstantly, NULL, 0, &unThreadId);

	// cast swapchain
	IDXGISwapChain* pcSwapChain = (IDXGISwapChain*)pThis;
	if (!pcSwapChain)
	{
		OutputDebugString(L"Oculus Direct Mode Node : No swapchain !");
		return nullptr;
	}
	// get device
	ID3D11Device* pcDevice = nullptr;
	pcSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pcDevice);
	if (!pcDevice)
	{
		OutputDebugString(L"Oculus Direct Mode Node : No d3d 11 device !");
		return nullptr;
	}
	// get context
	ID3D11DeviceContext* pcContext = nullptr;
	pcDevice->GetImmediateContext(&pcContext);
	if (!pcContext)
	{
		OutputDebugString(L"Oculus Direct Mode Node : No device context !");
		return nullptr;
	}

	if (m_ppHMD)
	{
		if (*m_ppHMD)
		{
			if (!m_bInit)
			{
#pragma region Init
				if (!m_pcDeviceTemporary)
				{
					// create temporary device
					IDXGIFactory * DXGIFactory;
					IDXGIAdapter * Adapter;
					if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory), (void**)(&DXGIFactory))))
						return(false);
					if (FAILED(DXGIFactory->EnumAdapters(0, &Adapter)))
						return(false);
					if (FAILED(D3D11CreateDevice(Adapter, Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
						NULL, 0, NULL, 0, D3D11_SDK_VERSION, &m_pcDeviceTemporary, NULL, &m_pcContextTemporary)))
						return(false);
					DXGIFactory->Release();

					// Set max frame latency to 1
					IDXGIDevice1* DXGIDevice1 = NULL;
					HRESULT hr = m_pcDeviceTemporary->QueryInterface(__uuidof(IDXGIDevice1), (void**)&DXGIDevice1);
					if (FAILED(hr) | (DXGIDevice1 == NULL))
					{
						pcSwapChain->Release();
						pcDevice->Release();
						pcContext->Release();
						return nullptr;
					}
					DXGIDevice1->SetMaximumFrameLatency(1);
					DXGIDevice1->Release();
				}

				// is the OpenVR compositor present ?
				if (!vr::VRCompositor())
				{
					OutputDebugString(L"OpenVR: Compositor initialization failed.\n");
					return nullptr;
				}

				// create the dashboard overlay
				vr::VROverlayError overlayError = vr::VROverlay()->CreateDashboardOverlay(OPENVR_OVERLAY_NAME, OPENVR_OVERLAY_FRIENDLY_NAME, &m_ulOverlayHandle, &m_ulOverlayThumbnailHandle);
				if (overlayError != vr::VROverlayError_None)
				{
					OutputDebugString(L"OpenVR: Failed to create overlay.");
					return nullptr;
				}

				// set overlay options
				vr::VROverlay()->SetOverlayWidthInMeters(m_ulOverlayHandle, m_sOverlayPropertiesDashboard.fWidth);
				vr::VROverlay()->SetOverlayInputMethod(m_ulOverlayHandle, vr::VROverlayInputMethod_Mouse);
				vr::VROverlay()->SetOverlayAlpha(m_ulOverlayHandle, m_sOverlayPropertiesDashboard.sColor.a);
				vr::VROverlay()->SetOverlayColor(m_ulOverlayHandle, m_sOverlayPropertiesDashboard.sColor.r, m_sOverlayPropertiesDashboard.sColor.g, m_sOverlayPropertiesDashboard.sColor.b);

				// create the HUD overlay
				overlayError = vr::VROverlay()->CreateOverlay(OPENVR_HUD_OVERLAY_NAME, OPENVR_HUD_OVERLAY_FRIENDLY_NAME, &m_ulHUDOverlayHandle);

				// set overlay options
				vr::HmdMatrix34_t sMatTransformHUD_OpenVR;
				sMatTransformHUD_OpenVR = Translate_3x4(m_sOverlayPropertiesHud.sVectorTranslation);
				switch (m_sOverlayPropertiesHud.eTransform)
				{
					case OpenVR_DirectMode::Absolute:
						vr::VROverlay()->SetOverlayTransformAbsolute(m_ulHUDOverlayHandle, m_sOverlayPropertiesHud.eOrigin, &sMatTransformHUD_OpenVR);
						break;
					case OpenVR_DirectMode::TrackedDeviceRelative:
						vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_ulHUDOverlayHandle, m_sOverlayPropertiesHud.nDeviceIndex, &sMatTransformHUD_OpenVR);
						break;
					case OpenVR_DirectMode::TrackedDeviceComponent:
						break;
				}
				vr::VROverlay()->SetOverlayWidthInMeters(m_ulHUDOverlayHandle, m_sOverlayPropertiesHud.fWidth);
				vr::VROverlay()->SetOverlayInputMethod(m_ulHUDOverlayHandle, vr::VROverlayInputMethod_Mouse);
				vr::VROverlay()->SetOverlayAlpha(m_ulHUDOverlayHandle, m_sOverlayPropertiesHud.sColor.a);
				vr::VROverlay()->SetOverlayColor(m_ulHUDOverlayHandle, m_sOverlayPropertiesHud.sColor.r, m_sOverlayPropertiesHud.sColor.g, m_sOverlayPropertiesHud.sColor.b);

				vr::VROverlay()->ShowOverlay(m_ulHUDOverlayHandle);

				m_bInit = true;
#pragma endregion
			}
			else
			if (vr::VRCompositor()->CanRenderScene())
			{
#pragma region Render overlay
#pragma region Dashboard overlay
				if (vr::VROverlay() && vr::VROverlay()->IsOverlayVisible(m_ulOverlayHandle))
				{
					// create all bool
					bool bAllCreated = true;

					// create vertex shader
					if (!m_pcVertexShader11)
					{
						if (FAILED(Create2DVertexShader(pcDevice, &m_pcVertexShader11, &m_pcVertexLayout11)))
						{
							bAllCreated = false;
						}
					}
					// create pixel shader... 
					if (!m_pcPixelShader11)
					{
						if (FAILED(CreateSimplePixelShader(pcDevice, &m_pcPixelShader11, PixelShaderTechnique::FullscreenSimple)))
							bAllCreated = false;
					}
					// Create vertex buffer
					if (!m_pcVertexBuffer11)
					{
						if (FAILED(CreateFullScreenVertexBuffer(pcDevice, &m_pcVertexBuffer11)))
							bAllCreated = false;
					}
					// create constant buffer
					if (!m_pcConstantBufferDirect11)
					{
						if (FAILED(CreateMatrixConstantBuffer(pcDevice, &m_pcConstantBufferDirect11)))
							bAllCreated = false;
					}
					// sampler ?
					if (!m_pcSamplerState)
					{
						// Create the sampler state
						D3D11_SAMPLER_DESC sSampDesc;
						ZeroMemory(&sSampDesc, sizeof(sSampDesc));
						sSampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
						sSampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
						sSampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
						sSampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
						sSampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
						sSampDesc.MinLOD = 0;
						sSampDesc.MaxLOD = D3D11_FLOAT32_MAX;
						if (FAILED(pcDevice->CreateSamplerState(&sSampDesc, &m_pcSamplerState)))
							bAllCreated = false;
					}

					// texture connected ?
					int nEye = 0;
					if ((m_ppcTexView11[nEye]) && (*m_ppcTexView11[nEye]))
					{
						// Set the input layout
						pcContext->IASetInputLayout(m_pcVertexLayout11);

						// Set vertex buffer
						UINT stride = sizeof(TexturedVertex);
						UINT offset = 0;
						pcContext->IASetVertexBuffers(0, 1, &m_pcVertexBuffer11, &stride, &offset);

						// Set constant buffer, first update it... scale and translate the left and right image
						D3DXMATRIX sProj;
						D3DXMatrixIdentity(&sProj);
						pcContext->UpdateSubresource((ID3D11Resource*)m_pcConstantBufferDirect11, 0, NULL, &sProj, 0, 0);
						pcContext->VSSetConstantBuffers(0, 1, &m_pcConstantBufferDirect11);

						// Set primitive topology
						pcContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

						if (bAllCreated)
						{

							if (!m_pcTex11Copy[nEye])
							{
								if (FAILED(CreateCopyTexture(pcDevice, m_pcDeviceTemporary, *m_ppcTexView11[nEye], &m_pcTex11Copy[nEye], &m_pcTex11Draw[nEye], &m_pcTex11DrawRTV[nEye], &m_pcTex11Shared[nEye])))
									return nullptr;
							}

							// set and clear render target
							FLOAT afColorRgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
							pcContext->ClearRenderTargetView(m_pcTex11DrawRTV[nEye], afColorRgba);
							pcContext->OMSetRenderTargets(1, &m_pcTex11DrawRTV[nEye], nullptr);

							// set texture, sampler state
							pcContext->PSSetShaderResources(0, 1, m_ppcTexView11[nEye]);
							pcContext->PSSetSamplers(0, 1, &m_pcSamplerState);

							// set shaders
							pcContext->VSSetShader(m_pcVertexShader11, 0, 0);
							pcContext->PSSetShader(m_pcPixelShader11, 0, 0);

							// Render a triangle
							pcContext->Draw(6, 0);

							// copy to copy texture
							pcContext->CopyResource(m_pcTex11Copy[nEye], m_pcTex11Draw[nEye]);

							// get shared handle.. for the Dashboard Overlay we CANNOT use m_pcTex11Shared for some reason !!
							IDXGIResource* pcDXGIResource(NULL);
							m_pcTex11Copy[nEye]->QueryInterface(__uuidof(IDXGIResource), (void**)&pcDXGIResource);
							HANDLE sharedHandle;
							if (pcDXGIResource)
							{
								pcDXGIResource->GetSharedHandle(&sharedHandle);
								pcDXGIResource->Release();
							}
							else OutputDebugString(L"Failed to query IDXGIResource.");

							ID3D11Resource* pcResourceShared;
							m_pcDeviceTemporary->OpenSharedResource(sharedHandle, __uuidof(ID3D11Resource), (void**)(&pcResourceShared));
							if (pcResourceShared)
							{
								// fill openvr texture struct
								vr::Texture_t sTexture = { (void*)pcResourceShared, vr::API_DirectX, vr::ColorSpace_Gamma };
								vr::VROverlay()->SetOverlayTexture(m_ulOverlayHandle, &sTexture);
								pcResourceShared->Release();
							}

						}
					}
				}
				else
#pragma endregion
#pragma region HUD overlay
				if ((!vr::VROverlay()->IsDashboardVisible()) && (vr::VROverlay()->IsOverlayVisible(m_ulHUDOverlayHandle)))
				{
					if (m_ppcTexViewHud11)
					{
						if (*m_ppcTexViewHud11)
						{
							// hud overlay shared textures present ?
							if (!m_pcTex11CopyHUD)
							{
								if (FAILED(CreateCopyTexture(pcDevice, m_pcDeviceTemporary, *m_ppcTexViewHud11, &m_pcTex11CopyHUD, nullptr, nullptr, &m_pcTex11SharedHUD)))
									return nullptr;
							}

							// get the resource texture from the HUD tex view
							ID3D11Texture2D* pcResource = nullptr;
							(*m_ppcTexViewHud11)->GetResource((ID3D11Resource**)&pcResource);

							if (pcResource)
							{
								// copy to copy texture
								pcContext->CopyResource(m_pcTex11CopyHUD, pcResource);

								// fill openvr texture struct
								vr::Texture_t sTexture = { (void*)m_pcTex11SharedHUD, vr::API_DirectX, vr::ColorSpace_Gamma };
								vr::VROverlay()->SetOverlayTexture(m_ulHUDOverlayHandle, &sTexture);
								pcResource->Release();
							}
						}
					}
				}
#pragma endregion
#pragma endregion
#pragma region Render
				if (!vr::VROverlay()->IsDashboardVisible())
				{
					// left + right
					for (int nEye = 0; nEye < 2; nEye++)
					{
						// texture connected ?
						if ((m_ppcTexView11[nEye]) && (*m_ppcTexView11[nEye]))
						{
							// are all textures created ?
							if (!m_pcTex11Copy[nEye])
							{
								if (FAILED(CreateCopyTexture(pcDevice, m_pcDeviceTemporary, *m_ppcTexView11[nEye], &m_pcTex11Copy[nEye], &m_pcTex11Draw[nEye], &m_pcTex11DrawRTV[nEye], &m_pcTex11Shared[nEye])))
									return nullptr;
							}

							// get the left game image texture resource
							ID3D11Resource* pcResource = nullptr;
							(*m_ppcTexView11[nEye])->GetResource(&pcResource);
							if (pcResource)
							{
								// copy resource
								pcContext->CopyResource(m_pcTex11Copy[nEye], pcResource);
								pcResource->Release();
							}
						}
					}
				}
#pragma endregion
			}
			else
				OutputDebugString(L"OpenVR: Current process has NOT the scene focus.");
		}
	}

	// release d3d11 device + context... 
	pcContext->Release();
	pcDevice->Release();

	return nullptr;
}