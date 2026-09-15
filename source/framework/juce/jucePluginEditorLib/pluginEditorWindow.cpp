#include "pluginEditorWindow.h"

#include "pluginEditor.h"
#include "pluginEditorState.h"

#include "dsp56kBase/logging.h"

#include "juceRmlPlugin/rmlParameterBinding.h"

#include "juceRmlUi/juceRmlComponent.h"

#include "RmlUi/Core/Elements/ElementFormControlInput.h"

namespace jucePluginEditorLib
{

//==============================================================================
EditorWindow::EditorWindow(juce::AudioProcessor& _p, PluginEditorState& _s, juce::PropertiesFile& _config)
	: AudioProcessorEditor(&_p), m_state(_s), m_config(_config)
{
	addMouseListener(this, true);

	m_state.evSkinLoaded = [&](juce::Component* _component)
	{
		setUiRoot(_component);
	};

	m_state.evSetGuiScale = [&](const int _scale)
	{
		if(getNumChildComponents() > 0)
			setGuiScale(static_cast<float>(_scale));
	};

	setUiRoot(m_state.getUiRoot());
}

EditorWindow::~EditorWindow()
{
	m_state.evSetGuiScale = [&](int){};
	m_state.evSkinLoaded = [&](juce::Component*){};

	setUiRoot(nullptr);
}

void EditorWindow::updateSizeConstraints()
{
	auto maxW = m_state.getWidth() * 4;
	auto maxH = m_state.getHeight() * 4;

#if JUCE_IOS
	/* Bound the constrainer by the screen. With a fixed aspect ratio and a maximum of
	 * 4x native, handing it a full-screen rectangle makes it satisfy the ratio by
	 * GROWING the width past the display edge -- measured on an iPad as a 1376x1032
	 * screen producing a 1961x1032 editor, ~585px hanging off the side. There is no
	 * window to drag on iOS, so a 4x maximum buys nothing.
	 *
	 * Recomputed on every resize, not once at setup: ROTATING the device swaps the
	 * display bounds, and a cap left at the previous orientation's numbers scales the
	 * panel wrongly for the new one. */
	if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
	{
		maxW = std::min(maxW, display->userArea.getWidth());
		maxH = std::min(maxH, display->userArea.getHeight());
	}
#endif

	m_sizeConstrainer.setMaximumSize(maxW, maxH);
}

void EditorWindow::resized()
{
#if JUCE_IOS
	// orientation may have changed since the last call
	updateSizeConstraints();
#endif

	AudioProcessorEditor::resized();

	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	const auto w = getWidth();
	const auto h = getHeight();

	const auto scaleX = static_cast<float>(w) / static_cast<float>(m_state.getWidth());
	const auto scaleY = static_cast<float>(h) / static_cast<float>(m_state.getHeight());

	const auto scale = std::min(scaleX, scaleY);

	/* Fit the skin INSIDE the space we were given and centre it, rather than
	 * handing the editor the raw bounds. The skin has a fixed aspect ratio; on
	 * the desktop the window constrainer enforces it, so fitW/fitH come back as
	 * w/h and this changes nothing. A host that dictates our bounds -- every iOS
	 * AUv3 -- hands us an arbitrary rectangle instead, and passing it through
	 * meant the UI was simply wider than the view and clipped at one edge.
	 * Letter/pillarboxing keeps the whole panel visible and undistorted. */
	const auto fitW = static_cast<int>(static_cast<float>(m_state.getWidth())  * scale);
	const auto fitH = static_cast<int>(static_cast<float>(m_state.getHeight()) * scale);

	if (!m_state.resizeEditor(fitW, fitH))
		return;

	if (auto* root = m_state.getUiRoot())
		root->setTopLeftPosition((w - fitW) / 2, (h - fitH) / 2);

	const auto percent = 100.f * scale / m_state.getRootScale();
	m_config.setValue("scale", percent);
	m_config.saveIfNeeded();

	// Prettymuch unbelievable Juce VST3 bug, but our root component is a child of the VST3 editor component
	// and that one is not resized! The host window is, the first child (our editor component) is, but the
	// root component is not! This is no drama as long as you do not have a juce OpenGL context, because
	// that one uses the "top level component" to set the clipping rectangle! W T F
	startTimer(1);
}

int EditorWindow::getControlParameterIndex(Component& _component)
{
	// This code relies on the fact that getComponentAt() is called with a XY position
	// first and then the parameter is queried for that returned component afterwards.
	// As we do not have Juce components, we remember the last Rml element that was
	// under the mouse and query the parameter binding for that element here.
	// It would be better if there was a function like "getParameterForPosition" but unfortunately
	// Juce does not provide that.
	if (const auto* editor = m_state.getEditor())
	{
		if (const auto* comp = editor->getRmlComponent())
		{
			if (const auto* binding = editor->getRmlParameterBinding())
			{
				if (const auto* elem = comp->getLastElementByGetComponentAt())
				{
					if (const auto* param = binding->getParameterForElement(elem))
						return param->getParameterIndex();

					if (const auto* parent = elem->GetParentNode())
					{
						if (dynamic_cast<const Rml::ElementFormControlInput*>(parent))
						{
							if (const auto* param = binding->getParameterForElement(parent))
								return param->getParameterIndex();
						}
					}
				}
			}
		}
	}

	return AudioProcessorEditor::getControlParameterIndex(_component);
}

void EditorWindow::setGuiScale(const float _percent)
{
	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	const auto s = _percent / 100.0f * m_state.getRootScale();

	auto w = static_cast<int>(static_cast<float>(m_state.getWidth()) * s);
	auto h = static_cast<int>(static_cast<float>(m_state.getHeight()) * s);

#if JUCE_IOS
	/* Never ask for more room than exists. On the desktop an oversized editor just
	 * makes a bigger window; in an iOS AUv3 the view is clipped to what the host
	 * gives us, and since resized() then sees OUR inflated bounds rather than the
	 * host's, its fit-to-size maths computes a scale of ~1 and does nothing. The
	 * panel ends up scaled to the height and cut off at one side.
	 *
	 * Cap against the parent if we are already in the hierarchy, otherwise against
	 * the display, which is the upper bound for any host view. */
	juce::Rectangle<int> avail;

	if (const auto* parent = getParentComponent())
		avail = parent->getLocalBounds();

	if (avail.isEmpty())
	{
		if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
			avail = display->userArea;
	}

	if (!avail.isEmpty() && w > 0 && h > 0)
	{
		const auto fit = std::min(static_cast<float>(avail.getWidth())  / static_cast<float>(w),
		                          static_cast<float>(avail.getHeight()) / static_cast<float>(h));
		if (fit < 1.0f)
		{
			w = static_cast<int>(static_cast<float>(w) * fit);
			h = static_cast<int>(static_cast<float>(h) * fit);
		}
	}
#endif

	setSize(w, h);

	m_config.setValue("scale", _percent);
	m_config.saveIfNeeded();
}

void EditorWindow::setUiRoot(juce::Component* _component)
{
	removeAllChildren();
	setConstrainer(nullptr);

	if(!_component)
		return;

	if(!m_state.getWidth() || !m_state.getHeight())
		return;

	m_sizeConstrainer.setMinimumSize(m_state.getWidth() / 10, m_state.getHeight() / 10);

	updateSizeConstraints();

	m_sizeConstrainer.setFixedAspectRatio(static_cast<double>(m_state.getWidth()) / static_cast<double>(m_state.getHeight()));
	
    const auto scale = static_cast<float>(m_config.getDoubleValue("scale", 100));
	setGuiScale(scale);

	_component->setSize(getWidth(), getHeight());

	addAndMakeVisible(_component);

	setResizable(true, true);
	setConstrainer(&m_sizeConstrainer);
}

void EditorWindow::timerCallback()
{
	fixParentWindowSize();
	stopTimer();
}

void EditorWindow::fixParentWindowSize() const
{
#if JUCE_IOS
	/* Growing the parent is a workaround for a JUCE VST3 bug (see the caller). On
	 * iOS the parent chain ends at a view the HOST owns and sizes; we cannot make
	 * it bigger, and trying leaves our component larger than the visible area, so
	 * the panel gets clipped at one edge. resized() fits and centres instead. */
	return;
#else
	const auto w = getWidth();
	const auto h = getHeight();

	auto* parent = getParentComponent();

	while (parent)
	{
		if (parent->getWidth() < w || parent->getHeight() < h)
		{
			LOG("Parent " << parent->getName() << " has wrong size: " << parent->getName() <<
				", expected: " << w << "x" << h <<
				", actual: " << parent->getWidth() << "x" << parent->getHeight());
			parent->setSize(w, h);
		}

		parent = parent->getParentComponent();
	}
#endif
}
}
