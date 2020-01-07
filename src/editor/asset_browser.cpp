#include <imgui/imgui.h>

#include "asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/render_interface.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/log.h"
#include "engine/path_utils.h"
#include "engine/os.h"
#include "engine/profiler.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/string.h"
#include "utils.h"


namespace Lumix
{


bool AssetBrowser::IPlugin::createTile(const char* in_path, const char* out_path, ResourceType type)
{
	return false;
}


AssetBrowser::AssetBrowser(StudioApp& app)
	: m_editor(app.getWorldEditor())
	, m_selected_resources(app.getWorldEditor().getAllocator())
	, m_is_focus_requested(false)
	, m_history(app.getWorldEditor().getAllocator())
	, m_plugins(app.getWorldEditor().getAllocator())
	, m_app(app)
	, m_is_open(false)
	, m_show_thumbnails(true)
	, m_history_index(-1)
	, m_file_infos(app.getWorldEditor().getAllocator())
	, m_filtered_file_infos(app.getWorldEditor().getAllocator())
	, m_subdirs(app.getWorldEditor().getAllocator())
{
	IAllocator& allocator = m_editor.getAllocator();
	m_filter[0] = '\0';

	const char* base_path = m_editor.getEngine().getFileSystem().getBasePath();

	StaticString<MAX_PATH_LENGTH> path(base_path, ".lumix");
	OS::makePath(path);
	path << "/asset_tiles";
	OS::makePath(path);

	m_back_action = LUMIX_NEW(allocator, Action)("Back", "Back in asset history", "back");
	m_back_action->is_global = false;
	m_back_action->func.bind<&AssetBrowser::goBack>(this);
	m_forward_action = LUMIX_NEW(allocator, Action)("Forward", "Forward in asset history", "forward");
	m_forward_action->is_global = false;
	m_forward_action->func.bind<&AssetBrowser::goForward>(this);
	m_app.addAction(m_back_action);
	m_app.addAction(m_forward_action);
}


AssetBrowser::~AssetBrowser()
{
	unloadResources();
	RenderInterface* ri = m_app.getWorldEditor().getRenderInterface();
	for (FileInfo& info : m_file_infos) {
		ri->unloadTexture(info.tex);
	}
	m_file_infos.clear();

	ASSERT(m_plugins.size() == 0);
}


void AssetBrowser::unloadResources()
{
	if (m_selected_resources.empty()) return;

	for (Resource* res : m_selected_resources) {
		for (auto* plugin : m_plugins) {
			plugin->onResourceUnloaded(res);
		}
		res->getResourceManager().unload(*res);
	}

	m_selected_resources.clear();
}


void AssetBrowser::update()
{
	PROFILE_FUNCTION();

	for (auto* plugin : m_plugins) plugin->update();
}


static void clampText(char* text, int width)
{
	char* end = text + stringLength(text);
	ImVec2 size = ImGui::CalcTextSize(text);
	if (size.x <= width) return;

	do
	{
		*(end - 1) = '\0';
		*(end - 2) = '.';
		*(end - 3) = '.';
		*(end - 4) = '.';
		--end;

		size = ImGui::CalcTextSize(text);
	} while (size.x > width && end - text > 4);
}

static const char* getResourceFilePath(const char* str)
{
	const char* c = str;
	while (*c && *c != ':') ++c;
	return *c != ':' ? str : c + 1;
}

static Span<const char> getSubresource(const char* str)
{
	Span<const char> ret;
	ret.m_begin = str;
	ret.m_end = str;
	while(*ret.m_end && *ret.m_end != ':') ++ret.m_end;
	return ret;
}

void AssetBrowser::changeDir(const char* path)
{
	WorldEditor& editor = m_app.getWorldEditor();
	RenderInterface* ri = editor.getRenderInterface();
	for (FileInfo& info : m_file_infos) {
		ri->unloadTexture(info.tex);
	}
	m_file_infos.clear();

	PathUtils::normalize(path, Span(m_dir.data));
	int len = stringLength(m_dir);
	if (len > 0 && (m_dir[len - 1] == '/' || m_dir[len - 1] == '\\')) {
		m_dir.data[len - 1] = '\0';
	}

	FileSystem& fs = editor.getEngine().getFileSystem();
	OS::FileIterator* iter = fs.createFileIterator(m_dir);
	OS::FileInfo info;
	m_subdirs.clear();
	while (OS::getNextFile(iter, &info)) {
		if (!info.is_directory) continue;
		if (info.filename[0] != '.') m_subdirs.emplace(info.filename);
	}
	OS::destroyFileIterator(iter);

	AssetCompiler& compiler = m_app.getAssetCompiler();
	const u32 dir_hash = crc32(m_dir);
	auto& resources = compiler.lockResources();
	for (const AssetCompiler::ResourceItem& res : resources) {
		if (res.dir_hash != dir_hash) continue;

		FileInfo tile;
		char filename[MAX_PATH_LENGTH];
		Span<const char> subres = getSubresource(res.path.c_str());
		if (*subres.end()) {
			copyNString(Span(filename), subres.begin(), subres.length());
			catString(filename, ":");
			const int tmp_len = stringLength(filename);
			PathUtils::getBasename(Span(filename + tmp_len, filename + sizeof(filename)), res.path.c_str());
		}
		else {
			PathUtils::getBasename(Span(filename), res.path.c_str());
		}
		clampText(filename, TILE_SIZE);

		tile.file_path_hash = res.path.getHash();
		tile.filepath = res.path.c_str();
		tile.clamped_filename = filename;

		m_file_infos.push(tile);
	}
	compiler.unlockResources();

	doFilter();
}


void AssetBrowser::breadcrumbs()
{
	const char* c = m_dir.data;
	char tmp[MAX_PATH_LENGTH];
	while (*c)
	{
		char* c_out = tmp;
		while (*c && *c != '/')
		{
			*c_out = *c;
			++c_out;
			++c;
		}
		*c_out = '\0';
		if (*c == '/') ++c;
		if (ImGui::Button(tmp))
		{
			char new_dir[MAX_PATH_LENGTH];
			copyNString(Span(new_dir), m_dir, int(c - m_dir.data));
			changeDir(new_dir);
		}
		ImGui::SameLine(0, 1);
		ImGui::Text("%s", "/");
		ImGui::SameLine(0, 1);
	}
	ImGui::NewLine();
}


void AssetBrowser::dirColumn()
{
	ImVec2 size(maximum(120.f, m_left_column_width), 0);
	ImGui::BeginChild("left_col", size);
	ImGui::PushItemWidth(120);
	bool b = false;
	if (ImGui::Selectable("..", &b))
	{
		char dir[MAX_PATH_LENGTH];
		PathUtils::getDir(Span(dir), m_dir);
		changeDir(dir);
	}

	for (auto& subdir : m_subdirs)
	{
		if (ImGui::Selectable(subdir, &b))
		{
			StaticString<MAX_PATH_LENGTH> new_dir(m_dir, "/", subdir);
			changeDir(new_dir);
		}
	}

	ImGui::PopItemWidth();
	ImGui::EndChild();
}


void AssetBrowser::doFilter()
{
	m_filtered_file_infos.clear();
	if (!m_filter[0]) return;

	for (int i = 0, c = m_file_infos.size(); i < c; ++i)
	{
		if (stristr(m_file_infos[i].filepath, m_filter)) m_filtered_file_infos.push(i);
	}
}


int AssetBrowser::getThumbnailIndex(int i, int j, int columns) const
{
	int idx = j * columns + i;
	if (!m_filtered_file_infos.empty())
	{
		if (idx >= m_filtered_file_infos.size()) return -1;
		return m_filtered_file_infos[idx];
	}
	if (idx >= m_file_infos.size())
	{
		return -1;
	}
	return idx;
}


void AssetBrowser::createTile(FileInfo& tile, const char* out_path)
{
	if (tile.create_called) return;
	
	logInfo("Editor") << "Creating tile for " << tile.filepath;
	tile.create_called = true;
	const AssetCompiler& compiler = m_app.getAssetCompiler();
	for (IPlugin* plugin : m_plugins) {
		ResourceType type = compiler.getResourceType(tile.filepath);
		if (plugin->createTile(tile.filepath, out_path, type)) break;
	}
}


void AssetBrowser::thumbnail(FileInfo& tile)
{
	ImGui::BeginGroup();
	ImVec2 img_size((float)TILE_SIZE, (float)TILE_SIZE);
	RenderInterface* ri = m_app.getWorldEditor().getRenderInterface();
	if (tile.tex)
	{
		if(ri->isValid(tile.tex)) {
			int* th = (int*)tile.tex;
			ImGui::Image((void*)(uintptr)*th, img_size);
		}
		else {
			ImGui::Dummy(img_size);
		}
	}
	else
	{
		ImGui::Rect(img_size.x, img_size.y, 0xffffFFFF);
		StaticString<MAX_PATH_LENGTH> path(".lumix/asset_tiles/", tile.file_path_hash, ".dds");
		FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
		if (fs.fileExists(path))
		{
			if (fs.getLastModified(path) >= fs.getLastModified(tile.filepath))
			{
				tile.tex = ri->loadTexture(Path(path));
			}
			else
			{
				createTile(tile, path);
			}
		}
		else
		{
			createTile(tile, path);
		}
	}
	ImVec2 text_size = ImGui::CalcTextSize(tile.clamped_filename);
	ImVec2 pos = ImGui::GetCursorPos();
	pos.x += (TILE_SIZE - text_size.x) * 0.5f;
	ImGui::SetCursorPos(pos);
	ImGui::Text("%s", tile.clamped_filename.data);
	ImGui::EndGroup();
}

void AssetBrowser::deleteTile(u32 idx) {
	FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
	if (!fs.deleteFile(m_file_infos[idx].filepath)) {
		logError("Editor") << "Failed to delete " << m_file_infos[idx].filepath;
	}
}

void AssetBrowser::fileColumn()
{
	ImGui::BeginChild("main_col");

	float w = ImGui::GetContentRegionAvail().x;
	int columns = m_show_thumbnails ? (int)w / TILE_SIZE : 1;
	columns = maximum(columns, 1);
	int tile_count = m_filtered_file_infos.empty() ? m_file_infos.size() : m_filtered_file_infos.size();
	int row_count = m_show_thumbnails ? (tile_count + columns - 1) / columns : tile_count;
	ImGuiListClipper clipper(row_count);
	
	auto callbacks = [this](FileInfo& tile, int idx) {
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tile.filepath.data);
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			ImGui::Text("%s", (const char*)tile.filepath);
			ImGui::SetDragDropPayload("path", tile.filepath, stringLength(tile.filepath) + 1, ImGuiCond_Once);
			ImGui::EndDragDropSource();
		}
		else if (ImGui::IsItemHovered())
		{
			if (ImGui::IsMouseReleased(0)) {
				const bool additive = OS::isKeyDown(OS::Keycode::LSHIFT);
				selectResource(Path(tile.filepath), true, additive);
			}
			else if(ImGui::IsMouseReleased(1)) {
				m_context_resource = idx;
				ImGui::OpenPopup("item_ctx");
			}
		}
	};

	while (clipper.Step())
	{
		for (int j = clipper.DisplayStart; j < clipper.DisplayEnd; ++j)
		{
			if (m_show_thumbnails)
			{
				for (int i = 0; i < columns; ++i)
				{
					if (i > 0) ImGui::SameLine();
					int idx = getThumbnailIndex(i, j, columns);
					if (idx < 0) break;
					FileInfo& tile = m_file_infos[idx];
					thumbnail(tile);
					callbacks(tile, idx);
				}
			}
			else
			{
				const int idx = (!m_filtered_file_infos.empty()) ? m_filtered_file_infos[j] : j;
				FileInfo& tile = m_file_infos[idx];
				bool b = m_selected_resources.find([&](Resource* res){ return res->getPath().getHash() == tile.file_path_hash; }) >= 0;
				ImGui::Selectable(tile.filepath, b);
				callbacks(tile, idx);
			}
		}
	}

	bool open_delete_popup = false;
	FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
	static char tmp[MAX_PATH_LENGTH] = "";
	auto common_popup = [&](){
		const char* base_path = fs.getBasePath();
		if (ImGui::MenuItem("View in explorer")) {
			StaticString<MAX_PATH_LENGTH> dir_full_path(base_path, "/", m_dir);
			OS::openExplorer(dir_full_path);
		}
		if (ImGui::BeginMenu("Create directory")) {
			ImGui::InputTextWithHint("##dirname", "New directory name", tmp, sizeof(tmp));
			ImGui::SameLine();
			if (ImGui::Button("Create")) {
				StaticString<MAX_PATH_LENGTH> path(base_path, "/", m_dir, "/", tmp);
				if (!OS::makePath(path)) {
					logError("Editor") << "Failed to create " << path;
				}
				changeDir(m_dir);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndMenu();
		}
		for (IPlugin* plugin : m_plugins) {
			if (!plugin->canCreateResource()) continue;
			if (ImGui::BeginMenu(plugin->getName())) {
				ImGui::InputTextWithHint("", "Name", tmp, sizeof(tmp));
				ImGui::SameLine();
				if (ImGui::Button("Create")) {
					StaticString<MAX_PATH_LENGTH> rel_path(m_dir, "/", tmp, ".", plugin->getDefaultExtension());
					StaticString<MAX_PATH_LENGTH> full_path(base_path, rel_path);
					plugin->createResource(full_path);
					changeDir(m_dir);
					m_wanted_resource = rel_path;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
		}
		if (ImGui::MenuItem("Select all")) {
			m_selected_resources.clear();
			m_selected_resources.reserve(m_file_infos.size());
			if(m_filtered_file_infos.empty()) {
				for (const FileInfo& fi : m_file_infos) {
					selectResource(Path(fi.filepath), false, true);
				}
			}
			else {
				for (int i : m_filtered_file_infos) {
					selectResource(Path(m_file_infos[i].filepath), false, true);
				}
			}
		}
	};
	if (ImGui::BeginPopup("item_ctx")) {
		ImGui::Text("%s", m_file_infos[m_context_resource].clamped_filename.data);
		ImGui::Separator();
		if (ImGui::MenuItem("Open in external editor")) {
			openInExternalEditor(m_file_infos[m_context_resource].filepath);
		}
		if (ImGui::BeginMenu("Rename")) {
			ImGui::InputTextWithHint("##New name", "New name", tmp, sizeof(tmp));
			if (ImGui::Button("Rename", ImVec2(100, 0))) {
				PathUtils::FileInfo fi(m_file_infos[m_context_resource].filepath);
				StaticString<MAX_PATH_LENGTH> new_path(fi.m_dir, tmp, ".", fi.m_extension);
				if (!fs.moveFile(m_file_infos[m_context_resource].filepath, new_path)) {
					logError("Editor") << "Failed to rename " << m_file_infos[m_context_resource].filepath << " to " << new_path;
				}
				else {
					changeDir(m_dir);
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndMenu();
		}
		open_delete_popup = ImGui::MenuItem("Delete");
		ImGui::Separator();
		common_popup();
		ImGui::EndPopup();
	}
	else if (ImGui::BeginPopupContextWindow("context")) {
		common_popup();
		ImGui::EndPopup();
	}

	if (open_delete_popup) ImGui::OpenPopup("Delete file");

	if (ImGui::BeginPopupModal("Delete file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Are you sure? This can not be undone.");
		if (ImGui::Button("Yes, delete", ImVec2(100, 0))) {
			deleteTile(m_context_resource);
			changeDir(m_dir);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine(ImGui::GetWindowWidth() - 100 - ImGui::GetStyle().WindowPadding.x);
		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::EndChild();
}


void AssetBrowser::detailsGUI()
{
	if (!m_is_open) return;
	if (m_is_focus_requested) ImGui::SetNextWindowFocus();
	m_is_focus_requested = false;
	if (ImGui::Begin("Asset properties", &m_is_open))
	{
		ImVec2 pos = ImGui::GetCursorScreenPos();
		if (ImGui::BeginToolbar("asset_browser_toolbar", pos, ImVec2(0, 24)))
		{
			if (m_history_index > 0) m_back_action->toolbarButton();
			if (m_history_index < m_history.size() - 1) m_forward_action->toolbarButton();
		}
		ImGui::EndToolbar();

		if (m_selected_resources.empty())
		{
			ImGui::End();
			return;
		}

		if (m_selected_resources.size() == 1) {
			Resource* res = m_selected_resources[0];
			const char* path = res->getPath().c_str();
			ImGui::Separator();
			ImGui::LabelText("Selected resource", "%s", path);
			ImGui::Separator();

			ImGui::LabelText("Status", "%s", res->isFailure() ? "failure" : (res->isReady() ? "Ready" : "Not ready"));
			if (res->isReady()) {
				ImGui::LabelText("Compiled size", "%.2f KB", res->size() / 1024.f);
			}
			const Span<const char> subres = getSubresource(m_selected_resources[0]->getPath().c_str());
			if (*subres.end()) {
				if (ImGui::Button("View parent")) {
					selectResource(Path(getResourceFilePath(m_selected_resources[0]->getPath().c_str())), true, false);
				}
			}

			const AssetCompiler& compiler = m_app.getAssetCompiler();
			ResourceType resource_type = compiler.getResourceType(path);
			auto iter = m_plugins.find(resource_type);
			if (iter.isValid()) {
				iter.value()->onGUI(m_selected_resources);
			}
		}
		else {
			ImGui::Separator();
			ImGui::LabelText("Selected resource", "%s", "multiple");
			ImGui::Separator();

			u32 ready = 0;
			u32 failed = 0;
			const ResourceType type = m_selected_resources[0]->getType();
			bool all_same_type = true;
			for (Resource* res : m_selected_resources) {
				ready += res->isReady() ? 1 : 0;
				failed += res->isFailure() ? 1 : 0;
				all_same_type = all_same_type && res->getType() == type;
			}

			ImGui::LabelText("All", "%d", m_selected_resources.size());
			ImGui::LabelText("Ready", "%d", ready);
			ImGui::LabelText("Failed", "%d", failed);

			if (all_same_type) {
				auto iter = m_plugins.find(type);
				if(iter.isValid()) {
					iter.value()->onGUI(m_selected_resources);
				}
			}
			else {
				ImGui::Text("Selected resources have different types.");
			}
		}
	}
	ImGui::End();
}


void AssetBrowser::onGUI()
{
	if (m_dir.data[0] == '\0') changeDir(".");
	
	if (m_wanted_resource.isValid())
	{
		selectResource(m_wanted_resource, true, false);
		m_wanted_resource = "";
	}

	m_is_open = m_is_open || m_is_focus_requested;

	if(m_is_open) {
		if (m_is_focus_requested) ImGui::SetNextWindowFocus();
		if (!ImGui::Begin("Assets", &m_is_open)) {
			ImGui::End();
			detailsGUI();
			return;
		}

		float checkbox_w = ImGui::GetCursorPosX();
		ImGui::Checkbox("Thumbnails", &m_show_thumbnails);
		ImGui::SameLine();
		checkbox_w = ImGui::GetCursorPosX() - checkbox_w;
		ImGui::PushItemWidth(100);
		if (ImGui::InputTextWithHint("##filter", "Filter", m_filter, sizeof(m_filter))) doFilter();
		ImGui::PopItemWidth();
		ImGui::SameLine(130 + checkbox_w);
		breadcrumbs();
		ImGui::Separator();

		float content_w = ImGui::GetContentRegionAvail().x;
		ImVec2 left_size(m_left_column_width, 0);
		if (left_size.x < 10) left_size.x = 10;
		if (left_size.x > content_w - 10) left_size.x = content_w - 10;
	
		dirColumn();

		ImGui::SameLine();
		ImGui::VSplitter("vsplit1", &left_size);
		if (left_size.x >= 120) {
			m_left_column_width = left_size.x;
		}
		ImGui::SameLine();

		fileColumn();

		ImGui::End();
	}
	
	detailsGUI();
}


void AssetBrowser::selectResource(Resource* resource, bool record_history, bool additive)
{
	if (record_history)
	{
		while (m_history_index < m_history.size() - 1)
		{
			m_history.pop();
		}
		m_history_index++;
		m_history.push(resource->getPath());

		if (m_history.size() > 20)
		{
			--m_history_index;
			m_history.erase(0);
		}
	}

	m_wanted_resource = "";
	if(additive) {
		if(m_selected_resources.indexOf(resource) >= 0) {
			m_selected_resources.swapAndPopItem(resource);
		}
		else {
			m_selected_resources.push(resource);
		}
	}
	else {
		unloadResources();
		m_selected_resources.push(resource);
	}
	ASSERT(resource->getRefCount() > 0);
}


void AssetBrowser::removePlugin(IPlugin& plugin)
{
	m_plugins.erase(plugin.getResourceType());
}


void AssetBrowser::addPlugin(IPlugin& plugin)
{
	m_plugins.insert(plugin.getResourceType(), &plugin);
}

static void copyDir(const char* src, const char* dest, IAllocator& allocator)
{
	PathUtils::FileInfo fi(src);
	StaticString<MAX_PATH_LENGTH> dst_dir(dest, "/", fi.m_basename);
	OS::makePath(dst_dir);
	OS::FileIterator* iter = OS::createFileIterator(src, allocator);

	OS::FileInfo cfi;
	while(OS::getNextFile(iter, &cfi)) {
		if (cfi.is_directory) {
			if (cfi.filename[0] != '.') {
				StaticString<MAX_PATH_LENGTH> tmp_src(src, "/", cfi.filename);
				StaticString<MAX_PATH_LENGTH> tmp_dst(dest, "/", fi.m_basename);
				copyDir(tmp_src, tmp_dst, allocator);
			}
		}
		else {
			StaticString<MAX_PATH_LENGTH> tmp_src(src, "/", cfi.filename);
			StaticString<MAX_PATH_LENGTH> tmp_dst(dest, "/", fi.m_basename, "/", cfi.filename);
			if(!OS::copyFile(tmp_src, tmp_dst)) {
				logError("Editor") << "Failed to copy " << tmp_src << " to " << tmp_dst;
			}
		}
	}
	OS::destroyFileIterator(iter);
}

bool AssetBrowser::onDropFile(const char* path)
{
	FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
	if (OS::dirExists(path)) {
		StaticString<MAX_PATH_LENGTH> tmp(fs.getBasePath(), "/", m_dir, "/");
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		copyDir(path, tmp, allocator);
	}
	PathUtils::FileInfo fi(path);
	StaticString<MAX_PATH_LENGTH> dest(fs.getBasePath(), "/", m_dir, "/", fi.m_basename, ".", fi.m_extension);
	return OS::copyFile(path, dest);
}

void AssetBrowser::selectResource(const Path& path, bool record_history, bool additive)
{
	m_is_focus_requested = true;
	auto& manager = m_editor.getEngine().getResourceManager();
	const AssetCompiler& compiler = m_app.getAssetCompiler();
	const ResourceType type = compiler.getResourceType(path.c_str());
	Resource* res = manager.load(type, path);
	if (res) selectResource(res, record_history, additive);
}

static StaticString<MAX_PATH_LENGTH> getImGuiLabelID(const ResourceLocator& rl, bool hash_id) {
	StaticString<MAX_PATH_LENGTH> res("");
	if (rl.full.length() > 0) {
		res << rl.subresource << (rl.subresource.length() > 0 ? ":" : "") << rl.basename << "." << rl.ext;
	}
	if (hash_id) {
		res << "##h" << crc32(rl.full.m_begin, rl.full.length());
	}
	return res;
}

bool AssetBrowser::resourceInput(const char* label, const char* str_id, Span<char> buf, ResourceType type)
{
	ImGui::PushID(str_id);
	float item_w = ImGui::CalcItemWidth();
	auto& style = ImGui::GetStyle();
	float text_width = maximum(50.0f, item_w - ImGui::CalcTextSize(" ... ").x - style.FramePadding.x * 2);
	
	auto pos = ImGui::GetCursorPos();
	pos.x += text_width;
	ImGui::BeginGroup();
	ImGui::AlignTextToFramePadding();
	ImGui::PushTextWrapPos(pos.x);

	const ResourceLocator rl(Span(buf.m_begin, stringLength(buf.m_begin)));
	ImGui::Text("%s", getImGuiLabelID(rl, false).data);

	ImGui::PopTextWrapPos();
	ImGui::SameLine();
	ImGui::SetCursorPos(pos);
	if (ImGui::Button(" ... ")) {
		ImGui::OpenPopup("popup");
	}
	ImGui::EndGroup();
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", buf.m_begin);
	}
	if (ImGui::BeginDragDropTarget()) {
		if (auto* payload = ImGui::AcceptDragDropPayload("path")) {
			char ext[10];
			const char* path = (const char*)payload->Data;
			Span<const char> subres = getSubresource(path);
			PathUtils::getExtension(Span(ext), subres);
			const AssetCompiler& compiler = m_app.getAssetCompiler();
			if (compiler.acceptExtension(ext, type)) {
				copyString(buf, path);
				ImGui::EndDragDropTarget();
				ImGui::PopID();
				return true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine();
	ImGui::Text("%s", label);

	if (ImGui::BeginResizablePopup("popup", ImVec2(300, 300))) {
		if (buf[0] != '\0' && ImGui::Button(StaticString<30>("View###go", str_id))) {
			m_is_focus_requested = true;
			m_is_open = true;
			m_wanted_resource = buf.begin();
		}
		if (ImGui::Selectable("Empty", false)) {
			buf[0] = '\0';
			ImGui::EndPopup();
			ImGui::PopID();
			return true;
		}
		static u32 selected_path_hash = 0;
		if (resourceList(buf, Ref(selected_path_hash), type, 0, true)) {
			ImGui::EndPopup();
			ImGui::PopID();
			return true;
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return false;
}


OutputMemoryStream* AssetBrowser::beginSaveResource(Resource& resource)
{
	IAllocator& allocator = m_app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, OutputMemoryStream)(allocator);
}


void AssetBrowser::endSaveResource(Resource& resource, OutputMemoryStream& stream, bool success)
{
	if (!success) return;
	
	FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
	// use temporary because otherwise the resource is reloaded during saving
	StaticString<MAX_PATH_LENGTH> tmp_path(resource.getPath().c_str(), ".tmp");
	OS::OutputFile f;
	if (!fs.open(tmp_path, Ref(f)))
	{
		LUMIX_DELETE(m_app.getWorldEditor().getAllocator(), &stream);
		logError("Editor") << "Could not save file " << resource.getPath().c_str();
		return;
	}
	f.write(stream.getData(), stream.getPos());
	f.close();
	LUMIX_DELETE(m_app.getWorldEditor().getAllocator(), &stream);

	auto& engine = m_app.getWorldEditor().getEngine();
	StaticString<MAX_PATH_LENGTH> src_full_path;
	StaticString<MAX_PATH_LENGTH> dest_full_path;
	src_full_path.data[0] = 0;
	dest_full_path.data[0] = 0;
	src_full_path << engine.getFileSystem().getBasePath() << tmp_path;
	dest_full_path << engine.getFileSystem().getBasePath() << resource.getPath().c_str();

	OS::deleteFile(dest_full_path);

	if (!OS::moveFile(src_full_path, dest_full_path))
	{
		logError("Editor") << "Could not save file " << resource.getPath().c_str();
	}
}

bool AssetBrowser::resourceList(Span<char> buf, Ref<u32> selected_path_hash, ResourceType type, float height, bool can_create_new) const {
	auto iter = m_plugins.find(type);
	if (!iter.isValid()) return false;

	IPlugin* plugin = iter.value();
	if (can_create_new && plugin->canCreateResource() && ImGui::Selectable("New")) {
		char full_path[MAX_PATH_LENGTH];
		if (OS::getSaveFilename(Span(full_path), plugin->getFileDialogFilter(), plugin->getFileDialogExtensions())) {
			if (plugin->createResource(full_path)) {
				m_editor.makeRelative(buf, full_path);
				return true;
			}
		}
	}

	static char filter[128] = "";
	ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));

	ImGui::BeginChild("Resources", ImVec2(0, height - ImGui::GetTextLineHeight() * 3), false, ImGuiWindowFlags_HorizontalScrollbar);
	AssetCompiler& compiler = m_app.getAssetCompiler();
	
	const auto& resources = compiler.lockResources();
	Path selected_path;
	for (const auto& res : resources) {
		if(res.type != type) continue;
		if (filter[0] != '\0' && strstr(res.path.c_str(), filter) == nullptr) continue;

		const bool selected = selected_path_hash == res.path.getHash();
		if(selected) selected_path = res.path;
		const ResourceLocator rl(Span(res.path.c_str(), res.path.length()));
		StaticString<MAX_PATH_LENGTH> label = getImGuiLabelID(rl, true);
		if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
			selected_path_hash = res.path.getHash();
			
			if (selected || ImGui::IsMouseDoubleClicked(0)) {
				copyString(buf, res.path.c_str());
				ImGui::CloseCurrentPopup();
				ImGui::EndChild();
				compiler.unlockResources();
				return true;
			}
		}
	}
	ImGui::EndChild();
	ImGui::Separator();
	if (selected_path.isValid()) {
		ImGui::Text("%s", selected_path.c_str());
	}
	compiler.unlockResources();
	return false;
}


void AssetBrowser::openInExternalEditor(Resource* resource) const
{
	openInExternalEditor(resource->getPath().c_str());
}


void AssetBrowser::openInExternalEditor(const char* path) const
{
	StaticString<MAX_PATH_LENGTH> full_path(m_editor.getEngine().getFileSystem().getBasePath());
	full_path << path;
	const OS::ExecuteOpenResult res = OS::shellExecuteOpen(full_path);
	if (res == OS::ExecuteOpenResult::NO_ASSOCIATION) {
		logError("Editor") << full_path << " is not associated with any app.";
	}
	else if (res == OS::ExecuteOpenResult::OTHER_ERROR) {
		logError("Editor") << "Failed to open " << full_path << " in exeternal editor.";
	}
}


void AssetBrowser::goBack()
{
	if (m_history_index < 1) return;
	m_history_index = maximum(0, m_history_index - 1);
	selectResource(m_history[m_history_index], false, false);
}


void AssetBrowser::goForward()
{
	m_history_index = minimum(m_history_index + 1, m_history.size() - 1);
	selectResource(m_history[m_history_index], false, false);
}


} // namespace Lumix