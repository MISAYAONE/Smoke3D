//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "pch.h"
#include "SharedMacros.h"
#include "Poisson3D.h"

using namespace DirectX;
using namespace DX;
using namespace std;
using namespace XSDX;

const auto g_pNullSRV = static_cast<LPDXShaderResourceView>(nullptr);	// Helper to Clear SRVs
const auto g_pNullUAV = static_cast<LPDXUnorderedAccessView>(nullptr);	// Helper to Clear UAVs
const auto g_uNullUint = 0u;											// Helper to Clear Buffers

Poisson3D::Poisson3D(const CPDXDevice &pDXDevice, const spShader &pShader, const spState &pState) :
	m_pDXDevice(pDXDevice),
	m_pShader(pShader),
	m_pState(pState),
	m_uCSIteration(0ui8),
	m_uCSTemporal(1ui8),
	m_uCSDiv(2ui8),
	m_uUASlot(0ui8),
	m_uSRField(0ui8),
	m_uSmpLinearClamp(1ui8)
{
	m_pDXDevice->GetImmediateContext(&m_pDXContext);
}

void Poisson3D::Init(const DirectX::XMUINT3 &vSimSize, const uint8_t uStride, const DXGI_FORMAT format)
{
	Init(vSimSize.x, vSimSize.y, vSimSize.z, uStride, format);
}

void Poisson3D::Init(const uint32_t uWidth, const uint32_t uHeight, const uint32_t uDepth,
	const uint8_t uStride, const DXGI_FORMAT format)
{
	// Initialize data
	const auto uByteWidth = uStride * uWidth * uHeight * uDepth;
	auto vData = vbyte(uByteWidth);
	ZeroMemory(vData.data(), uByteWidth);

	// Create 3D textures
	m_pSrcKnown = make_shared<Texture3D>(m_pDXDevice);
	m_pDstUnknown = make_shared<Texture3D>(m_pDXDevice);
	m_pSrcKnown->Create(true, false, uWidth, uHeight, uDepth, format);
	m_pDstUnknown->Create(true, false, uWidth, uHeight, uDepth, format, 1ui8, vData.data(), uStride);

	if (format != DXGI_FORMAT_R32_FLOAT && format != DXGI_FORMAT_R16_FLOAT)
	{
		m_pSrcUnknown = make_shared<Texture3D>(m_pDXDevice);
		m_pSrcUnknown->Create(true, false, uWidth, uHeight, uDepth, format, 1ui8, vData.data(), uStride);
	}
	else m_pSrcUnknown = nullptr;

	m_vThreadGroupSize = XMUINT3(uWidth / THREAD_BLOCK_X, uHeight / THREAD_BLOCK_Y, uDepth / THREAD_BLOCK_Z);
}

void Poisson3D::SetShaders(const uint8_t uCSIteration, const uint8_t uCSTemporal, const uint8_t uCSDiv)
{
	m_uCSIteration = uCSIteration;
	m_uCSTemporal = uCSTemporal;
	m_uCSDiv = uCSDiv;
}

void Poisson3D::ComputeDivergence(const CPDXShaderResourceView &srvSource)
{
	// Setup
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, m_pDstUnknown->GetUAV().GetAddressOf(), &g_uNullUint);
	m_pDXContext->CSSetShaderResources(m_uSRField, 1u, srvSource.GetAddressOf());

	// Compute Divergence
	m_pDXContext->CSSetShader(m_pShader->GetComputeShader(m_uCSDiv).Get(), nullptr, 0u);
	m_pDXContext->Dispatch(m_vThreadGroupSize.x, m_vThreadGroupSize.y, m_vThreadGroupSize.z);

	// Unset
	m_pDXContext->CSSetShaderResources(m_uSRField, 1u, &g_pNullSRV);
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, &g_pNullUAV, &g_uNullUint);

	// Swap buffers
	SwapBuffers();
}

void Poisson3D::SolvePoisson(const uint8_t uIteration)
{
	// Setup
	m_pDXContext->CSSetShaderResources(m_uSRField, 1u, m_pSrcKnown->GetSRV().GetAddressOf());

	// Iterations
	if (m_pSrcUnknown) for (auto i = 0ui8; i < uIteration; ++i) jacobi();
	else gaussSeidel();

	// Unset
	const auto pSRVs = array<LPDXShaderResourceView, 2>{ { nullptr, nullptr } };
	m_pDXContext->CSSetShaderResources(m_uSRField, 2u, pSRVs.data());
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, &g_pNullUAV, &g_uNullUint);

	// Swap buffers
	SwapBuffers();
}

void Poisson3D::Advect(const CPDXShaderResourceView &srvSource)
{
	// Setup
	const auto pSRVs = array<LPDXShaderResourceView, 2>
	{ { m_pSrcKnown->GetSRV().Get(), srvSource.Get() } };
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, m_pDstUnknown->GetUAV().GetAddressOf(), &g_uNullUint);
	m_pDXContext->CSSetShaderResources(m_uSRField, 2u, pSRVs.data());
	m_pDXContext->CSSetSamplers(m_uSmpLinearClamp, 1u, m_pState->LinearClamp().GetAddressOf());

	// Compute Divergence
	m_pDXContext->CSSetShader(m_pShader->GetComputeShader(m_uCSTemporal).Get(), nullptr, 0u);
	m_pDXContext->Dispatch(m_vThreadGroupSize.x, m_vThreadGroupSize.y, m_vThreadGroupSize.z);

	// Unset
	const auto pNullSRVs = array<LPDXShaderResourceView, 2>{ { nullptr, nullptr } };
	m_pDXContext->CSSetShaderResources(m_uSRField, 2u, pNullSRVs.data());
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, &g_pNullUAV, &g_uNullUint);

	// Swap buffers
	SwapBuffers();
}

void Poisson3D::SwapBuffers(bool bUnknown)
{
	if (bUnknown) m_pSrcUnknown.swap(m_pDstUnknown);
	else m_pSrcKnown.swap(m_pDstUnknown);
}

const spTexture3D &Poisson3D::GetSrc() const
{
	return m_pSrcKnown;
}

const spTexture3D &Poisson3D::GetDst() const
{
	return m_pDstUnknown;
}

const spTexture3D &Poisson3D::GetTmp() const
{
	return m_pSrcUnknown;
}

void Poisson3D::gaussSeidel()
{
	// Setup
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, m_pDstUnknown->GetUAV().GetAddressOf(), &g_uNullUint);

	// Iterations
	m_pDXContext->CSSetShader(m_pShader->GetComputeShader(m_uCSIteration).Get(), nullptr, 0u);
	m_pDXContext->Dispatch(m_vThreadGroupSize.x, m_vThreadGroupSize.y, m_vThreadGroupSize.z);
}

void Poisson3D::jacobi()
{
	// Setup
	m_pDXContext->CSSetUnorderedAccessViews(m_uUASlot, 1u, m_pDstUnknown->GetUAV().GetAddressOf(), &g_uNullUint);
	m_pDXContext->CSSetShaderResources(m_uSRField + 1u, 1u, m_pSrcUnknown->GetSRV().GetAddressOf());

	// Jacobi iteration
	m_pDXContext->CSSetShader(m_pShader->GetComputeShader(m_uCSIteration).Get(), nullptr, 0u);
	m_pDXContext->Dispatch(m_vThreadGroupSize.x, m_vThreadGroupSize.y, m_vThreadGroupSize.z);

	// Unset
	m_pDXContext->CSSetShaderResources(m_uSRField + 1u, 1u, &g_pNullSRV);

	// Swap buffers
	m_pSrcUnknown.swap(m_pDstUnknown);
}
