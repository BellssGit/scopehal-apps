/***********************************************************************************************************************
*                                                                                                                      *
* ngscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2024 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of ProtocolAnalyzerDialog
 */

#include "ngscopeclient.h"
#include "ProtocolAnalyzerDialog.h"
#include "MainWindow.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

ProtocolAnalyzerDialog::ProtocolAnalyzerDialog(
	PacketDecoder* filter, shared_ptr<PacketManager> mgr, Session& session, MainWindow& wnd)
	: Dialog(
		string("Protocol: ") + filter->GetDisplayName(),
		string("Protocol: ") + filter->GetHwname(),
		ImVec2(425, 350))
	, m_filter(filter)
	, m_mgr(mgr)
	, m_session(session)
	, m_parent(wnd)
	, m_waveformChanged(false)
	, m_lastSelectedWaveform(0, 0)
	, m_selectedPacket(nullptr)
	, m_dataFormat(FORMAT_HEX)
	, m_needToScrollToSelectedPacket(false)
	, m_firstDataBlockOfFrame(true)
	, m_bytesPerLine(1)
{
	//Hold a reference open to the filter so it doesn't disappear on us
	m_filter->AddRef();
}

ProtocolAnalyzerDialog::~ProtocolAnalyzerDialog()
{
	m_filter->Release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

/**
	@brief Renders the dialog and handles UI events

	@return		True if we should continue showing the dialog
				False if it's been closed
 */
bool ProtocolAnalyzerDialog::DoRender()
{
	static ImGuiTableFlags flags =
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_SizingFixedFit;

	float width = ImGui::GetFontSize();

	auto cols = m_filter->GetHeaders();
	//TODO: hide certain headers like length and ascii?

	//Figure out channel setup
	//Default is timestamp plus all headers, add optional other channels as needed
	int ncols = 1 + cols.size();
	int datacol = 0;
	if(m_filter->GetShowDataColumn())
		datacol = (ncols ++);
	if(m_filter->GetShowImageColumn())
		ncols ++;
	//TODO: integrate length natively vs having to make the filter calculate it??

	auto dataFont = m_parent.GetFontPref("Appearance.Protocol Analyzer.data_font");

	//Figure out color for filter expression
	ImU32 bgcolor;
	size_t ifilter = 0;
	ProtocolDisplayFilter filter(m_filterExpression, ifilter);
	if(m_filterExpression == "")
		bgcolor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
	else if(filter.Validate(cols))
		bgcolor = ColorFromString("#008000");
	else
		bgcolor = ColorFromString("#800000");
	//TODO: yellow for possibly wrong stuff?
	//TODO: allow configuration under preferences

	//Filter expression
	float boxwidth = ImGui::GetContentRegionAvail().x;
	ImGui::SetNextItemWidth(boxwidth - ImGui::CalcTextSize("Filter").x - ImGui::GetStyle().ItemSpacing.x);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, bgcolor);
	ImGui::InputText("Filter", &m_filterExpression);
	bool updated = !ImGui::IsItemActive();
	bool filterDirty = (m_committedFilterExpression != m_filterExpression);
	ImGui::PopStyleColor();

	//Output format for data column
	//If this is changed force a refresh
	bool forceRefresh = false;
	if(m_filter->GetShowDataColumn())
	{
		ImGui::SetNextItemWidth(10 * width);
		if(ImGui::Combo("Data Format", (int*)&m_dataFormat, "Hex\0ASCII\0Hexdump\0"))
			forceRefresh = true;
	}

	//TODO: refresh after detecting columns resized, or is that not necessary?
	//TODO: refresh after detecting tree node expanded/shrunk

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// culling redo start

	m_firstDataBlockOfFrame = true;
	if(ImGui::BeginTable("table", ncols, flags))
	{
		ImGui::TableSetupScrollFreeze(0, 1); //Header row does not scroll
		ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 12*width);
		for(auto c : cols)
			ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthFixed, 0.0f);
		if(m_filter->GetShowDataColumn())
			ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch, 0.0f);
		if(m_filter->GetShowImageColumn())
			ImGui::TableSetupColumn("Image", ImGuiTableColumnFlags_WidthFixed, 0.0f);
		ImGui::TableHeadersRow();

		//Do an update cycle to make sure any recently acquired packets are captured
		m_mgr->Update();

		lock_guard lock(m_mgr->GetMutex());
		auto packets = m_mgr->GetFilteredPackets();

		//Make a list of waveform timestamps and make sure we display them in order
		vector<TimePoint> times;
		for(auto& it : packets)
			times.push_back(it.first);
		std::sort(times.begin(), times.end());

		//Process packets from each waveform
		for(auto wavetime : times)
		{
			//TODO: add some kind of marker to indicate gaps between waveforms (if we have >1)?
			ImGui::PushID(wavetime.first);
			ImGui::PushID(wavetime.second);

			auto& wpackets = packets[wavetime];
			for(auto pack : wpackets)
			{
				pack->RefreshColors();

				//Instead of using packet pointer as identifier (can change if filter graph re-runs for
				//unrelated reasons), use timestamp instead.
				ImGui::PushID(pack->m_offset);

				ImGui::TableNextRow(ImGuiTableRowFlags_None);

				//Set up colors for the packet
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, pack->m_displayBackgroundColorPacked);
				ImGui::PushStyleColor(ImGuiCol_Text, pack->m_displayForegroundColorPacked);

				//See if we have child packets
				auto children = m_mgr->GetFilteredChildPackets(pack);
				bool hasChildren = !children.empty();

				//Timestamp (and row selection logic)
				ImGui::TableSetColumnIndex(0);
				bool open = false;
				if(hasChildren)
				{
					open = ImGui::TreeNodeEx("##tree", ImGuiTreeNodeFlags_OpenOnArrow);

					if(m_lastChildOpen[pack] != open)
					{
						m_lastChildOpen[pack] = open;
						LogTrace("tree node opened or closed, forcing refresh\n");
						forceRefresh = true;
					}

					if(open)
						ImGui::TreePop();
					ImGui::SameLine();
				}
				bool rowIsSelected = (m_selectedPacket == pack);
				TimePoint packtime(wavetime.GetSec(), wavetime.GetFs() + pack->m_offset);
				if(ImGui::Selectable(
					packtime.PrettyPrint().c_str(),
					rowIsSelected,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
					ImVec2(0, 0)))
				{
					m_selectedPacket = pack;
					rowIsSelected = true;

					//See if a new waveform was selected
					if( (m_lastSelectedWaveform != TimePoint(0, 0)) && (m_lastSelectedWaveform != wavetime) )
						m_waveformChanged = true;
					m_lastSelectedWaveform = wavetime;

					m_parent.NavigateToTimestamp(pack->m_offset, pack->m_len, StreamDescriptor(m_filter, 0));

				}

				//Update scroll position if requested
				if(rowIsSelected && m_needToScrollToSelectedPacket)
				{
					m_needToScrollToSelectedPacket = false;
					ImGui::SetScrollHereY();
				}

				//Headers
				for(size_t i=0; i<cols.size(); i++)
				{
					if(ImGui::TableSetColumnIndex(i+1))
						ImGui::TextUnformatted(pack->m_headers[cols[i]].c_str());
				}

				//Data column
				if(m_filter->GetShowDataColumn())
				{
					if(DoDataColumn(datacol, pack, dataFont))
						forceRefresh = true;
				}

				//Child nodes for merged packets
				if(open)
				{
					ImGui::TreePush("##tree");

					for(auto child : children)
					{
						//Instead of using packet pointer as identifier (can change if filter graph re-runs for
						//unrelated reasons), use timestamp instead.
						ImGui::PushID(child->m_offset);

						ImGui::TableNextRow(ImGuiTableRowFlags_None);

						//Set up colors for the packet
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ColorFromString(child->m_displayBackgroundColor));
						ImGui::PushStyleColor(ImGuiCol_Text, ColorFromString(child->m_displayForegroundColor));

						bool childIsSelected = (m_selectedPacket == child);

						ImGui::TableSetColumnIndex(0);
						TimePoint ctime(wavetime.GetSec(), wavetime.GetFs() + child->m_offset);
						if(ImGui::Selectable(
							ctime.PrettyPrint().c_str(),
							childIsSelected,
							ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
							ImVec2(0, 0)))
						{
							m_selectedPacket = child;
							childIsSelected = true;

							//See if a new waveform was selected
							if( (m_lastSelectedWaveform != TimePoint(0, 0)) && (m_lastSelectedWaveform != wavetime) )
								m_waveformChanged = true;
							m_lastSelectedWaveform = wavetime;

							m_parent.NavigateToTimestamp(child->m_offset, child->m_len, StreamDescriptor(m_filter, 0));
						}

						//Update scroll position if requested
						if(rowIsSelected && m_needToScrollToSelectedPacket)
						{
							m_needToScrollToSelectedPacket = false;
							ImGui::SetScrollHereY();
						}

						//Headers
						for(size_t i=0; i<cols.size(); i++)
						{
							if(ImGui::TableSetColumnIndex(i+1))
								ImGui::TextUnformatted(child->m_headers[cols[i]].c_str());
						}

						//Data column
						if(m_filter->GetShowDataColumn())
						{
							if(DoDataColumn(datacol, child, dataFont))
								forceRefresh = true;
						}

						ImGui::PopStyleColor();
						ImGui::PopID();
					}
					ImGui::TreePop();
				}

				ImGui::PopStyleColor();
				ImGui::PopID();
			}

			ImGui::PopID();
			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	//culling redo end
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	//Apply filter expressions
	if( (updated && filterDirty) || forceRefresh)
	{
		if(!forceRefresh)
			m_committedFilterExpression = m_filterExpression;

		//No filter expression? Nothing to do
		if(m_filterExpression == "")
			m_mgr->SetDisplayFilter(nullptr);
		else
		{
			//Parse the expression. Apply only if valid
			//If not valid, keep old filter active
			ifilter = 0;
			auto pfilter = make_shared<ProtocolDisplayFilter>(m_filterExpression, ifilter);
			if(pfilter->Validate(cols))
				m_mgr->SetDisplayFilter(pfilter);
		}

		//TODO: remove entries in m_lastChildOpen and m_lastDataOpen for anything we don't have anymore
	}

	return true;
}

/**
	@brief Handles the "data" column for packets

	@return true if we need to refresh cached row heights because we opened/closed a tree
 */
bool ProtocolAnalyzerDialog::DoDataColumn(int datacol, Packet* pack, ImFont* dataFont)
{
	bool forceRefresh = false;

	if(ImGui::TableSetColumnIndex(datacol))
	{
		//When drawing the first cell, figure out dimensions for subsequent stuff
		if(m_firstDataBlockOfFrame)
		{
			//Available space (after subtracting tree button)
			auto xsize = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().IndentSpacing;

			//Figure out how many characters of text we can fit in the data region
			//This assumes data font is fixed width, may break if user chooses variable width.
			//But hex dumps with variable width will look horrible anyway so that's probably not a problem?
			auto fontwidth = dataFont->CalcTextSizeA(dataFont->FontSize, FLT_MAX, -1, "W").x;
			size_t charsPerLine = floor(xsize / fontwidth);

			//TODO: use 2-nibble address if packet has <256 bytes of data

			//Number of characters available for displaying data (address column doesn't count)
			size_t dataCharsPerLine = charsPerLine - 5;

			switch(m_dataFormat)
			{
				//Ascii is trivial: data bytes map 1:1 to characters
				case FORMAT_ASCII:
					m_bytesPerLine = dataCharsPerLine;
					break;

				//Hex needs three chars (2 hex + space)
				//TODO: last char doesn't need the space
				case FORMAT_HEX:
					m_bytesPerLine = dataCharsPerLine / 3;
					break;

				//Hexdump needs a fixed 3 spaces between the hex and the ascii parts.
				//Then we need 3 for each hex and one for each ascii.
				case FORMAT_HEXDUMP:
					m_bytesPerLine = (dataCharsPerLine - 3) / 4;
					break;
			}

			if(m_bytesPerLine <= 0)
				return forceRefresh;
		}

		string firstLine;

		auto& bytes = pack->m_data;

		string lineHex;
		string lineAscii;

		//Create the tree node early - before we've even rendered any data - so we know the open / closed state
		ImGui::PushFont(dataFont);
		bool open = false;
		if(!bytes.empty())
		{
			//If we have more than one line worth of data, show the tree
			if(bytes.size() > m_bytesPerLine)
			{
				open = ImGui::TreeNodeEx("##data", ImGuiTreeNodeFlags_OpenOnArrow);

				if(m_lastDataOpen[pack] != open)
				{
					m_lastDataOpen[pack] = open;
					LogTrace("data node opened or closed, forcing refresh\n");
					forceRefresh = true;
				}

				ImGui::SameLine();
			}
		}

		//Format the data
		string data;
		char tmp[32];
		for(size_t i=0; i<bytes.size(); i++)
		{
			//Address block
			if( (i % m_bytesPerLine) == 0)
			{
				//Is this the first block of an open tree view? Show address
				if(open)
				{
					snprintf(tmp, sizeof(tmp), "%04zx ", i);
					data += tmp;
				}

				//Tree closed or single line: don't show the 0000 which can be confused with data
				else
					data += "     ";
			}

			switch(m_dataFormat)
			{
				case FORMAT_HEX:
					snprintf(tmp, sizeof(tmp), "%02x ", bytes[i]);
					data += tmp;
					break;

				case FORMAT_ASCII:
					if(isprint(bytes[i]) || (bytes[i] == ' '))
						data += bytes[i];
					else
						data += '.';
					break;

				case FORMAT_HEXDUMP:

					//hex dump
					snprintf(tmp, sizeof(tmp), "%02x ", bytes[i]);
					lineHex += tmp;

					//ascii
					if(isprint(bytes[i]) || (bytes[i] == ' '))
						lineAscii += bytes[i];
					else
						lineAscii += '.';
					break;
			}

			if( (i % m_bytesPerLine) == m_bytesPerLine-1)
			{
				//Special processing for hex dump
				if(m_dataFormat == FORMAT_HEXDUMP)
				{
					data += lineHex + "   " + lineAscii;
					lineHex = "";
					lineAscii = "";
				}

				if(firstLine.empty())
				{
					firstLine = data;
					data = "";
				}
				else
					data += "\n";
			}
		}

		//Handle data less than one line in size
		if(firstLine.empty() && !data.empty())
		{
			firstLine = data;
			data = "";
		}

		if(m_dataFormat == FORMAT_HEXDUMP)
		{
			//process last partial line at end
			if(!lineHex.empty())
			{
				while(lineHex.length() < 3*m_bytesPerLine)
					lineHex += ' ';

				data += lineHex + "   " + lineAscii;
			}
		}

		ImGui::TextUnformatted(firstLine.c_str());

		//Multiple lines? Only show if open
		if(open)
		{
			ImGui::TextUnformatted(data.c_str());
			ImGui::TreePop();
		}

		ImGui::PopFont();
		m_firstDataBlockOfFrame = false;
	}

	return forceRefresh;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// UI event handlers

/**
	@brief Notifies the dialog that a cursor has been moved
 */
void ProtocolAnalyzerDialog::OnCursorMoved(int64_t offset)
{
	//If nothing is selected, use our current waveform timestamp as a reference
	if(m_lastSelectedWaveform == TimePoint(0, 0))
	{
		auto data = m_filter->GetData(0);
		m_lastSelectedWaveform = TimePoint(data->m_startTimestamp, data->m_startFemtoseconds);
	}

	auto& allpackets = m_mgr->GetFilteredPackets();
	auto it = allpackets.find(m_lastSelectedWaveform);
	if(it == allpackets.end())
		return;
	auto packets = it->second;

	//TODO: binary search vs linear
	for(auto p : packets)
	{
		//Check child packets first
		auto& children = m_mgr->GetFilteredChildPackets(p);
		for(auto c : children)
		{
			if(offset > (c->m_offset + c->m_len) )
				continue;
			if(c->m_offset > offset)
				return;

			m_selectedPacket = c;
			m_needToScrollToSelectedPacket = true;
			return;
		}

		//If we get here no child hit, try to match parent
		if(offset > (p->m_offset + p->m_len) )
			continue;
		if(p->m_offset > offset)
			return;

		m_selectedPacket = p;
		m_needToScrollToSelectedPacket = true;
		return;
	}
}
