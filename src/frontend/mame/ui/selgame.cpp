// license:BSD-3-Clause
// copyright-holders:Maurizio Petrarota
/*********************************************************************

    ui/selgame.cpp

    Main UI menu.

*********************************************************************/

#include "emu.h"
#include "ui/selgame.h"

#include "ui/auditmenu.h"
#include "ui/inifile.h"
#include "ui/miscmenu.h"
#include "ui/optsmenu.h"
#include "ui/selector.h"
#include "ui/selsoft.h"
#include "ui/ui.h"

#include "../info.h"

#include "audit.h"
#include "drivenum.h"
#include "emuopts.h"
#include "mame.h"
#include "rendutil.h"
#include "softlist_dev.h"
#include "uiinput.h"
#include "luaengine.h"


extern const char UI_VERSION_TAG[];

namespace ui {

bool menu_select_game::first_start = true;
std::vector<const game_driver *> menu_select_game::m_sortedlist;
int menu_select_game::m_isabios = 0;

//-------------------------------------------------
//  ctor
//-------------------------------------------------

menu_select_game::menu_select_game(mame_ui_manager &mui, render_container &container, const char *gamename)
	: menu_select_launch(mui, container, false)
{
	std::string error_string, last_filter, sub_filter;
	ui_options &moptions = mui.options();

	// load drivers cache
	init_sorted_list();

	// check if there are available icons
	ui_globals::has_icons = false;
	file_enumerator path(moptions.icons_directory());
	const osd::directory::entry *dir;
	while ((dir = path.next()) != nullptr)
	{
		std::string src(dir->name);
		if (src.find(".ico") != std::string::npos || src.find("icons") != std::string::npos)
		{
			ui_globals::has_icons = true;
			break;
		}
	}

	// build drivers list
	if (!load_available_machines())
		build_available_list();

	if (first_start)
	{
		reselect_last::set_driver(moptions.last_used_machine());
		ui_globals::rpanel = std::min<int>(std::max<int>(moptions.last_right_panel(), RP_FIRST), RP_LAST);

		std::string tmp(moptions.last_used_filter());
		std::size_t const found = tmp.find_first_of(",");
		std::string fake_ini;
		if (found == std::string::npos)
		{
			fake_ini = util::string_format("%s = 1\n", tmp);
		}
		else
		{
			std::string const sub_filter(tmp.substr(found + 1));
			tmp.resize(found);
			fake_ini = util::string_format("%s = %s\n", tmp, sub_filter);
		}

		emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
		if (file.open_ram(fake_ini.c_str(), fake_ini.size()) == osd_file::error::NONE)
		{
			machine_filter::ptr flt(machine_filter::create(file));
			if (flt)
			{
				main_filters::actual = flt->get_type();
				main_filters::filters.emplace(main_filters::actual, std::move(flt));
			}
			file.close();
		}
	}

	// do this after processing the last used filter setting so it overwrites the placeholder
	load_custom_filters();
	m_filter_highlight = main_filters::actual;

	if (!moptions.remember_last())
		reselect_last::reset();

	mui.machine().options().set_value(OPTION_SNAPNAME, "%g/%i", OPTION_PRIORITY_CMDLINE);

	ui_globals::curimage_view = FIRST_VIEW;
	ui_globals::curdats_view = 0;
	ui_globals::switch_image = false;
	ui_globals::default_image = true;
	ui_globals::panels_status = moptions.hide_panels();
	ui_globals::curdats_total = 1;
	m_searchlist[0] = nullptr;
}

//-------------------------------------------------
//  dtor
//-------------------------------------------------

menu_select_game::~menu_select_game()
{
	std::string error_string, last_driver;
	game_driver const *const driver(isfavorite() ? nullptr : reinterpret_cast<game_driver const *>(get_selection_ref()));
	ui_software_info *const swinfo(isfavorite() ? reinterpret_cast<ui_software_info *>(get_selection_ref()) : nullptr);

	if (reinterpret_cast<uintptr_t>(driver) > skip_main_items)
		last_driver = driver->name;
	else if (driver && m_prev_selected)
		last_driver = reinterpret_cast<game_driver const *>(m_prev_selected)->name;

	if (reinterpret_cast<uintptr_t>(swinfo) > skip_main_items)
		last_driver = swinfo->shortname;
	else if (swinfo && m_prev_selected)
		last_driver = reinterpret_cast<ui_software_info *>(m_prev_selected)->shortname;

	std::string filter;
	auto const active_filter(main_filters::filters.find(main_filters::actual));
	if (main_filters::filters.end() != active_filter)
	{
		char const *val(active_filter->second->filter_text());
		filter = val ? util::string_format("%s,%s", active_filter->second->config_name(), val) : active_filter->second->config_name();
	}
	else
	{
		filter = machine_filter::config_name(main_filters::actual);
	}

	ui_options &mopt = ui().options();
	mopt.set_value(OPTION_LAST_RIGHT_PANEL, ui_globals::rpanel, OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_LAST_USED_FILTER, filter.c_str(), OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_LAST_USED_MACHINE, last_driver.c_str(), OPTION_PRIORITY_CMDLINE);
	mopt.set_value(OPTION_HIDE_PANELS, ui_globals::panels_status, OPTION_PRIORITY_CMDLINE);
	ui().save_ui_options();
}

//-------------------------------------------------
//  handle
//-------------------------------------------------

void menu_select_game::handle()
{
	if (!m_prev_selected)
		m_prev_selected = item[0].ref;

	// if I have to load datfile, perform a hard reset
	if (ui_globals::reset)
	{
		ui_globals::reset = false;
		machine().schedule_hard_reset();
		stack_reset();
		return;
	}

	// if i have to reselect a software, force software list submenu
	if (reselect_last::get())
	{
		const game_driver *driver;
		const ui_software_info *software;
		get_selection(software, driver);
		menu::stack_push<menu_select_software>(ui(), container(), driver);
		return;
	}

	// ignore pause keys by swallowing them before we process the menu
	machine().ui_input().pressed(IPT_UI_PAUSE);

	// process the menu
	const event *menu_event = process(PROCESS_LR_REPEAT);
	if (menu_event)
	{
		if (dismiss_error())
		{
			// reset the error on any future menu_event
		}
		else switch (menu_event->iptkey)
		{
		case IPT_UI_UP:
			if ((get_focus() == focused_menu::LEFT) && (machine_filter::FIRST < m_filter_highlight))
				--m_filter_highlight;
			break;

		case IPT_UI_DOWN:
			if ((get_focus() == focused_menu::LEFT) && (machine_filter::LAST > m_filter_highlight))
				m_filter_highlight++;
			break;

		case IPT_UI_HOME:
			if (get_focus() == focused_menu::LEFT)
				m_filter_highlight = machine_filter::FIRST;
			break;

		case IPT_UI_END:
			if (get_focus() == focused_menu::LEFT)
				m_filter_highlight = machine_filter::LAST;
			break;

		case IPT_UI_CONFIGURE:
			inkey_navigation();
			break;

		case IPT_UI_EXPORT:
			inkey_export();
			break;

		case IPT_UI_DATS:
			inkey_dats();
			break;

		default:
			if (menu_event->itemref)
			{
				switch (menu_event->iptkey)
				{
				case IPT_UI_SELECT:
					if (get_focus() == focused_menu::MAIN)
					{
						if (isfavorite())
							inkey_select_favorite(menu_event);
						else
							inkey_select(menu_event);
					}
					break;

				case IPT_CUSTOM:
					// handle IPT_CUSTOM (mouse right click)
					if (!isfavorite())
					{
						menu::stack_push<menu_machine_configure>(
								ui(), container(),
								reinterpret_cast<const game_driver *>(m_prev_selected),
								menu_event->mouse.x0, menu_event->mouse.y0);
					}
					else
					{
						ui_software_info *sw = reinterpret_cast<ui_software_info *>(m_prev_selected);
						menu::stack_push<menu_machine_configure>(
								ui(), container(),
								(const game_driver *)sw->driver,
								menu_event->mouse.x0, menu_event->mouse.y0);
					}
					break;

				case IPT_UI_LEFT:
					if (ui_globals::rpanel == RP_IMAGES && ui_globals::curimage_view > FIRST_VIEW)
					{
						// Images
						ui_globals::curimage_view--;
						ui_globals::switch_image = true;
						ui_globals::default_image = false;
					}
					else if (ui_globals::rpanel == RP_INFOS)
					{
						// Infos
						change_info_pane(-1);
					}
					break;

				case IPT_UI_RIGHT:
					if (ui_globals::rpanel == RP_IMAGES && ui_globals::curimage_view < LAST_VIEW)
					{
						// Images
						ui_globals::curimage_view++;
						ui_globals::switch_image = true;
						ui_globals::default_image = false;
					}
					else if (ui_globals::rpanel == RP_INFOS)
					{
						// Infos
						change_info_pane(1);
					}
					break;

				case IPT_UI_FAVORITES:
					if (uintptr_t(menu_event->itemref) > skip_main_items)
					{
						favorite_manager &mfav(mame_machine_manager::instance()->favorite());
						if (!isfavorite())
						{
							game_driver const *const driver(reinterpret_cast<game_driver const *>(menu_event->itemref));
							if (!mfav.isgame_favorite(driver))
							{
								mfav.add_favorite_game(driver);
								machine().popmessage(_("%s\n added to favorites list."), driver->type.fullname());
							}
							else
							{
								mfav.remove_favorite_game();
								machine().popmessage(_("%s\n removed from favorites list."), driver->type.fullname());
							}
						}
						else
						{
							ui_software_info const *const swinfo(reinterpret_cast<ui_software_info const *>(menu_event->itemref));
							machine().popmessage(_("%s\n removed from favorites list."), swinfo->longname);
							mfav.remove_favorite_game(*swinfo);
							reset(reset_options::SELECT_FIRST);
						}
					}
					break;

				case IPT_UI_AUDIT_FAST:
					if (!m_unavailsortedlist.empty())
						menu::stack_push<menu_audit>(ui(), container(), m_availsortedlist, m_unavailsortedlist, 1);
					break;

				case IPT_UI_AUDIT_ALL:
					menu::stack_push<menu_audit>(ui(), container(), m_availsortedlist, m_unavailsortedlist, 2);
					break;
				}
			}
		}
	}

	// if we're in an error state, overlay an error message
	draw_error_text();
}

//-------------------------------------------------
//  populate
//-------------------------------------------------

void menu_select_game::populate(float &customtop, float &custombottom)
{
	ui_globals::redraw_icon = true;
	ui_globals::switch_image = true;
	int old_item_selected = -1;
	uint32_t flags_ui = FLAG_LEFT_ARROW | FLAG_RIGHT_ARROW;

	if (!isfavorite())
	{
		// if search is not empty, find approximate matches
		if (!m_search.empty())
		{
			populate_search();
		}
		else
		{
			// reset search string
			m_search.clear();
			m_displaylist.clear();

			// if filter is set on category, build category list
			switch (main_filters::actual)
			{
			case machine_filter::ALL:
				m_displaylist = m_sortedlist;
				break;
			case machine_filter::AVAILABLE:
				m_displaylist = m_availsortedlist;
				break;
			case machine_filter::UNAVAILABLE:
				m_displaylist = m_unavailsortedlist;
				break;
			default:
				{
					auto const it(main_filters::filters.find(main_filters::actual));
					std::copy_if(
							m_sortedlist.begin(),
							m_sortedlist.end(),
							std::back_inserter(m_displaylist),
							[&flt = *it->second] (game_driver const *drv) { return flt.apply(*drv); });
				}
			}

			// iterate over entries
			int curitem = 0;
			for (auto & elem : m_displaylist)
			{
				if (old_item_selected == -1 && elem->name == reselect_last::driver())
					old_item_selected = curitem;

				bool cloneof = strcmp(elem->parent, "0");
				if (cloneof)
				{
					int cx = driver_list::find(elem->parent);
					if (cx != -1 && ((driver_list::driver(cx).flags & machine_flags::IS_BIOS_ROOT) != 0))
						cloneof = false;
				}

				item_append(elem->type.fullname(), "", (cloneof) ? (flags_ui | FLAG_INVERT) : flags_ui, (void *)elem);
				curitem++;
			}
		}
	}
	else
	{
		// populate favorites list
		m_search.clear();
		int curitem = 0;

		// iterate over entries
		for (auto & favmap : mame_machine_manager::instance()->favorite().m_list)
		{
			auto flags = flags_ui | FLAG_UI_FAVORITE;
			if (favmap.second.startempty == 1)
			{
				if (old_item_selected == -1 && favmap.second.shortname == reselect_last::driver())
					old_item_selected = curitem;

				bool cloneof = strcmp(favmap.second.driver->parent, "0");
				if (cloneof)
				{
					int cx = driver_list::find(favmap.second.driver->parent);
					if (cx != -1 && ((driver_list::driver(cx).flags & machine_flags::IS_BIOS_ROOT) != 0))
						cloneof = false;
				}

				item_append(favmap.second.longname, "", (cloneof) ? (flags | FLAG_INVERT) : flags, (void *)&favmap.second);
			}
			else
			{
				if (old_item_selected == -1 && favmap.second.shortname == reselect_last::driver())
					old_item_selected = curitem;
				item_append(favmap.second.longname, favmap.second.devicetype,
							favmap.second.parentname.empty() ? flags : (FLAG_INVERT | flags), (void *)&favmap.second);
			}
			curitem++;
		}
	}

	item_append(menu_item_type::SEPARATOR, flags_ui);

	// add special items
	if (stack_has_special_main_menu())
	{
		item_append(_("Configure Options"), "", flags_ui, (void *)(uintptr_t)CONF_OPTS);
		item_append(_("Configure Machine"), "", flags_ui, (void *)(uintptr_t)CONF_MACHINE);
		skip_main_items = 2;
		if (machine().options().plugins())
		{
			item_append(_("Plugins"), "", flags_ui, (void *)(uintptr_t)CONF_PLUGINS);
			skip_main_items++;
		}
	}
	else
		skip_main_items = 0;

	// configure the custom rendering
	customtop = 3.0f * ui().get_line_height() + 5.0f * UI_BOX_TB_BORDER;
	custombottom = 5.0f * ui().get_line_height() + 3.0f * UI_BOX_TB_BORDER;

	// reselect prior game launched, if any
	if (old_item_selected != -1)
	{
		selected = old_item_selected;
		if (ui_globals::visible_main_lines == 0)
			top_line = (selected != 0) ? selected - 1 : 0;
		else
			top_line = selected - (ui_globals::visible_main_lines / 2);

		if (reselect_last::software().empty())
			reselect_last::reset();
	}
	else
	{
		reselect_last::reset();
	}
}

//-------------------------------------------------
//  build a list of available drivers
//-------------------------------------------------

void menu_select_game::build_available_list()
{
	int m_total = driver_list::total();
	std::vector<bool> m_included(m_total, false);

	// open a path to the ROMs and find them in the array
	file_enumerator path(machine().options().media_path());
	const osd::directory::entry *dir;

	// iterate while we get new objects
	while ((dir = path.next()) != nullptr)
	{
		char drivername[50];
		char *dst = drivername;
		const char *src;

		// build a name for it
		for (src = dir->name; *src != 0 && *src != '.' && dst < &drivername[ARRAY_LENGTH(drivername) - 1]; ++src)
			*dst++ = tolower((uint8_t) * src);

		*dst = 0;
		int drivnum = driver_list::find(drivername);
		if (drivnum != -1 && !m_included[drivnum])
		{
			m_availsortedlist.push_back(&driver_list::driver(drivnum));
			m_included[drivnum] = true;
		}
	}

	// now check and include NONE_NEEDED
	if (!ui().options().hide_romless())
	{
		for (int x = 0; x < m_total; ++x)
		{
			auto driver = &driver_list::driver(x);
			if (!m_included[x] && driver != &GAME_NAME(___empty))
			{
				auto entries = rom_build_entries(driver->rom);
				const rom_entry *rom = entries.data();
				bool noroms = true;

				// check NO-DUMP
				for (; !ROMENTRY_ISEND(rom) && noroms == true; ++rom)
					if (ROMENTRY_ISFILE(rom))
					{
						util::hash_collection hashes(ROM_GETHASHDATA(rom));
						if (!hashes.flag(util::hash_collection::FLAG_NO_DUMP) && !ROM_ISOPTIONAL(rom))
							noroms = false;
					}

				if (!noroms)
				{
					// check if clone == parent
					auto cx = driver_list::clone(*driver);
					if (cx != -1 && m_included[cx])
					{
						auto drv = &driver_list::driver(cx);
						if (driver->rom == drv->rom)
							noroms = true;

						// check if clone < parent
						if (!noroms)
						{
							noroms = true;
							for (; !ROMENTRY_ISEND(rom) && noroms == true; ++rom)
							{
								if (ROMENTRY_ISFILE(rom))
								{
									util::hash_collection hashes(ROM_GETHASHDATA(rom));
									if (hashes.flag(util::hash_collection::FLAG_NO_DUMP) || ROM_ISOPTIONAL(rom))
										continue;

									uint64_t lenght = ROM_GETLENGTH(rom);
									auto found = false;
									auto parent_entries = rom_build_entries(drv->rom);
									for (auto parentrom = parent_entries.data(); !ROMENTRY_ISEND(parentrom) && found == false; ++parentrom)
									{
										if (ROMENTRY_ISFILE(parentrom) && ROM_GETLENGTH(parentrom) == lenght)
										{
											util::hash_collection parenthashes(ROM_GETHASHDATA(parentrom));
											if (parenthashes.flag(util::hash_collection::FLAG_NO_DUMP) || ROM_ISOPTIONAL(parentrom))
												continue;

											if (hashes == parenthashes)
												found = true;
										}
									}
									noroms = found;
								}
							}
						}
					}
				}

				if (noroms)
				{
					m_availsortedlist.push_back(&driver_list::driver(x));
					m_included[x] = true;
				}
			}
		}
	}
	// sort
	std::stable_sort(m_availsortedlist.begin(), m_availsortedlist.end(), sorted_game_list);

	// now build the unavailable list
	for (int x = 0; x < m_total; ++x)
		if (!m_included[x] && &driver_list::driver(x) != &GAME_NAME(___empty))
			m_unavailsortedlist.push_back(&driver_list::driver(x));

	// sort
	std::stable_sort(m_unavailsortedlist.begin(), m_unavailsortedlist.end(), sorted_game_list);
}


//-------------------------------------------------
//  force the game select menu to be visible
//  and inescapable
//-------------------------------------------------

void menu_select_game::force_game_select(mame_ui_manager &mui, render_container &container)
{
	// reset the menu stack
	menu::stack_reset(mui.machine());

	// add the quit entry followed by the game select entry
	menu::stack_push_special_main<menu_quit_game>(mui, container);
	menu::stack_push<menu_select_game>(mui, container, nullptr);

	// force the menus on
	mui.show_menu();

	// make sure MAME is paused
	mui.machine().pause();
}

//-------------------------------------------------
//  handle select key event
//-------------------------------------------------

void menu_select_game::inkey_select(const event *menu_event)
{
	const game_driver *driver = (const game_driver *)menu_event->itemref;

	if ((uintptr_t)driver == CONF_OPTS)
	{
		// special case for configure options
		menu::stack_push<menu_game_options>(ui(), container());
	}
	else if (uintptr_t(driver) == CONF_MACHINE)
	{
		// special case for configure machine
		if (m_prev_selected)
			menu::stack_push<menu_machine_configure>(ui(), container(), reinterpret_cast<const game_driver *>(m_prev_selected));
		return;
	}
	else if ((uintptr_t)driver == CONF_PLUGINS)
	{
		// special case for configure plugins
		menu::stack_push<menu_plugins_configure>(ui(), container());
	}
	else
	{
		// anything else is a driver

		// audit the game first to see if we're going to work
		driver_enumerator enumerator(machine().options(), *driver);
		enumerator.next();
		media_auditor auditor(enumerator);
		media_auditor::summary const summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

		// if everything looks good, schedule the new driver
		if (summary == media_auditor::CORRECT || summary == media_auditor::BEST_AVAILABLE || summary == media_auditor::NONE_NEEDED)
		{
			for (software_list_device &swlistdev : software_list_device_iterator(enumerator.config()->root_device()))
			{
				if (!swlistdev.get_info().empty())
				{
					menu::stack_push<menu_select_software>(ui(), container(), driver);
					return;
				}
			}

			if (!select_bios(*driver, false))
				launch_system(*driver);
		}
		else
		{
			// otherwise, display an error
			set_error(reset_options::REMEMBER_REF, make_error_text(media_auditor::NOTFOUND != summary, auditor));
		}
	}
}

//-------------------------------------------------
//  handle select key event for favorites menu
//-------------------------------------------------

void menu_select_game::inkey_select_favorite(const event *menu_event)
{
	ui_software_info *ui_swinfo = (ui_software_info *)menu_event->itemref;

	if ((uintptr_t)ui_swinfo == CONF_OPTS)
	{
		// special case for configure options
		menu::stack_push<menu_game_options>(ui(), container());
	}
	else if ((uintptr_t)ui_swinfo == CONF_MACHINE)
	{
		// special case for configure machine
		if (m_prev_selected)
		{
			ui_software_info *swinfo = reinterpret_cast<ui_software_info *>(m_prev_selected);
			menu::stack_push<menu_machine_configure>(ui(), container(), (const game_driver *)swinfo->driver);
		}
		return;
	}
	else if ((uintptr_t)ui_swinfo == CONF_PLUGINS)
	{
		// special case for configure plugins
		menu::stack_push<menu_plugins_configure>(ui(), container());
	}
	else if (ui_swinfo->startempty == 1)
	{
		// audit the game first to see if we're going to work
		driver_enumerator enumerator(machine().options(), *ui_swinfo->driver);
		enumerator.next();
		media_auditor auditor(enumerator);
		media_auditor::summary const summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

		if (summary == media_auditor::CORRECT || summary == media_auditor::BEST_AVAILABLE || summary == media_auditor::NONE_NEEDED)
		{
			for (software_list_device &swlistdev : software_list_device_iterator(enumerator.config()->root_device()))
			{
				if (!swlistdev.get_info().empty())
				{
					menu::stack_push<menu_select_software>(ui(), container(), ui_swinfo->driver);
					return;
				}
			}

			// if everything looks good, schedule the new driver
			if (!select_bios(*ui_swinfo->driver, false))
			{
				reselect_last::reselect(true);
				launch_system(*ui_swinfo->driver);
			}
		}
		else
		{
			// otherwise, display an error
			set_error(reset_options::REMEMBER_REF, make_error_text(media_auditor::NOTFOUND != summary, auditor));
		}
	}
	else
	{
		// first validate
		driver_enumerator drv(machine().options(), *ui_swinfo->driver);
		media_auditor auditor(drv);
		drv.next();
		software_list_device *swlist = software_list_device::find_by_name(*drv.config(), ui_swinfo->listname.c_str());
		const software_info *swinfo = swlist->find(ui_swinfo->shortname.c_str());

		media_auditor::summary const summary = auditor.audit_software(swlist->list_name(), swinfo, AUDIT_VALIDATE_FAST);

		if (summary == media_auditor::CORRECT || summary == media_auditor::BEST_AVAILABLE || summary == media_auditor::NONE_NEEDED)
		{
			if (!select_bios(*ui_swinfo, false) && !select_part(*swinfo, *ui_swinfo))
				launch_system(drv.driver(), *ui_swinfo, ui_swinfo->part);
		}
		else
		{
			// otherwise, display an error
			set_error(reset_options::REMEMBER_POSITION, make_error_text(media_auditor::NOTFOUND != summary, auditor));
		}
	}
}

//-------------------------------------------------
//  returns if the search can be activated
//-------------------------------------------------

bool menu_select_game::isfavorite() const
{
	return machine_filter::FAVORITE == main_filters::actual;
}


//-------------------------------------------------
//  change what's displayed in the info box
//-------------------------------------------------

void menu_select_game::change_info_pane(int delta)
{
	auto const cap_delta = [this, &delta] (uint8_t &current, uint8_t &total)
	{
		if ((0 > delta) && (-delta > current))
			delta = -int(unsigned(current));
		else if ((0 < delta) && ((current + unsigned(delta)) >= total))
			delta = int(unsigned(total - current - 1));
		if (delta)
		{
			current += delta;
			m_topline_datsview = 0;
		}
	};
	game_driver const *drv;
	ui_software_info const *soft;
	get_selection(soft, drv);
	if (!isfavorite())
	{
		if (uintptr_t(drv) > skip_main_items)
			cap_delta(ui_globals::curdats_view, ui_globals::curdats_total);
	}
	else if (uintptr_t(soft) > skip_main_items)
	{
		if (soft->startempty)
			cap_delta(ui_globals::curdats_view, ui_globals::curdats_total);
		else
			cap_delta(ui_globals::cur_sw_dats_view, ui_globals::cur_sw_dats_total);
	}
}

//-------------------------------------------------
//  populate search list
//-------------------------------------------------

void menu_select_game::populate_search()
{
	// allocate memory to track the penalty value
	std::vector<int> penalty(VISIBLE_GAMES_IN_SEARCH, 9999);
	int index = 0;
	for (; index < m_displaylist.size(); ++index)
	{
		// pick the best match between driver name and description
		int curpenalty = fuzzy_substring(m_search, m_displaylist[index]->type.fullname());
		int tmp = fuzzy_substring(m_search, m_displaylist[index]->name);
		curpenalty = std::min(curpenalty, tmp);

		// insert into the sorted table of matches
		for (int matchnum = VISIBLE_GAMES_IN_SEARCH - 1; matchnum >= 0; --matchnum)
		{
			// stop if we're worse than the current entry
			if (curpenalty >= penalty[matchnum])
				break;

			// as long as this isn't the last entry, bump this one down
			if (matchnum < VISIBLE_GAMES_IN_SEARCH - 1)
			{
				penalty[matchnum + 1] = penalty[matchnum];
				m_searchlist[matchnum + 1] = m_searchlist[matchnum];
			}

			m_searchlist[matchnum] = m_displaylist[index];
			penalty[matchnum] = curpenalty;
		}
	}

	(index < VISIBLE_GAMES_IN_SEARCH) ? m_searchlist[index] = nullptr : m_searchlist[VISIBLE_GAMES_IN_SEARCH] = nullptr;
	uint32_t flags_ui = FLAG_LEFT_ARROW | FLAG_RIGHT_ARROW;
	for (int curitem = 0; m_searchlist[curitem]; ++curitem)
	{
		bool cloneof = strcmp(m_searchlist[curitem]->parent, "0");
		if (cloneof)
		{
			int cx = driver_list::find(m_searchlist[curitem]->parent);
			if (cx != -1 && ((driver_list::driver(cx).flags & machine_flags::IS_BIOS_ROOT) != 0))
				cloneof = false;
		}
		item_append(m_searchlist[curitem]->type.fullname(), "", (!cloneof) ? flags_ui : (FLAG_INVERT | flags_ui),
			(void *)m_searchlist[curitem]);
	}
}

//-------------------------------------------------
//  generate general info
//-------------------------------------------------

void menu_select_game::general_info(const game_driver *driver, std::string &buffer)
{
	system_flags const &flags(get_system_flags(*driver));
	std::ostringstream str;

	str << "#j2\n";

	util::stream_format(str, _("Romset\t%1$-.100s\n"), driver->name);
	util::stream_format(str, _("Year\t%1$s\n"), driver->year);
	util::stream_format(str, _("Manufacturer\t%1$-.100s\n"), driver->manufacturer);

	int cloneof = driver_list::non_bios_clone(*driver);
	if (cloneof != -1)
		util::stream_format(str, _("Driver is Clone of\t%1$-.100s\n"), driver_list::driver(cloneof).type.fullname());
	else
		str << _("Driver is Parent\t\n");

	if (flags.has_analog())
		str << _("Analog Controls\tYes\n");
	if (flags.has_keyboard())
		str << _("Keyboard Inputs\tYes\n");

	if (flags.machine_flags() & machine_flags::NOT_WORKING)
		str << _("Overall\tNOT WORKING\n");
	else if ((flags.unemulated_features() | flags.imperfect_features()) & device_t::feature::PROTECTION)
		str << _("Overall\tUnemulated Protection\n");
	else
		str << _("Overall\tWorking\n");

	if (flags.unemulated_features() & device_t::feature::GRAPHICS)
		str << _("Graphics\tUnimplemented\n");
	else if (flags.unemulated_features() & device_t::feature::PALETTE)
		str << ("Graphics\tWrong Colors\n");
	else if (flags.imperfect_features() & device_t::feature::PALETTE)
		str << _("Graphics\tImperfect Colors\n");
	else if (flags.imperfect_features() & device_t::feature::GRAPHICS)
		str << _("Graphics\tImperfect\n");
	else
		str << _("Graphics\tOK\n");

	if (flags.machine_flags() & machine_flags::NO_SOUND_HW)
		str << _("Sound\tNone\n");
	else if (flags.unemulated_features() & device_t::feature::SOUND)
		str << _("Sound\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::SOUND)
		str << _("Sound\tImperfect\n");
	else
		str << _("Sound\tOK\n");

	if (flags.unemulated_features() & device_t::feature::CONTROLS)
		str << _("Controls\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::CONTROLS)
		str << _("Controls\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::KEYBOARD)
		str << _("Keyboard\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::KEYBOARD)
		str << _("Keyboard\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::MOUSE)
		str << _("Mouse\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::MOUSE)
		str << _("Mouse\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::MICROPHONE)
		str << _("Microphone\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::MICROPHONE)
		str << _("Microphone\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::CAMERA)
		str << _("Camera\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::CAMERA)
		str << _("Camera\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::DISK)
		str << _("Disk\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::DISK)
		str << _("Disk\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::PRINTER)
		str << _("Printer\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::PRINTER)
		str << _("Printer\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::LAN)
		str << _("LAN\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::LAN)
		str << _("LAN\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::WAN)
		str << _("WAN\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::WAN)
		str << _("WAN\tImperfect\n");

	if (flags.unemulated_features() & device_t::feature::TIMING)
		str << _("Timing\tUnimplemented\n");
	else if (flags.imperfect_features() & device_t::feature::TIMING)
		str << _("Timing\tImperfect\n");

	util::stream_format(str, _("Mechanical Machine\t%1$s\n"), ((flags.machine_flags() & machine_flags::MECHANICAL) ? _("Yes") : _("No")));
	util::stream_format(str, _("Requires Artwork\t%1$s\n"), ((flags.machine_flags() & machine_flags::REQUIRES_ARTWORK) ? _("Yes") : _("No")));
	util::stream_format(str, _("Requires Clickable Artwork\t%1$s\n"), ((flags.machine_flags() & machine_flags::CLICKABLE_ARTWORK) ? _("Yes") : _("No")));
	util::stream_format(str, _("Support Cocktail\t%1$s\n"), ((flags.machine_flags() & machine_flags::NO_COCKTAIL) ? _("Yes") : _("No")));
	util::stream_format(str, _("Driver is BIOS\t%1$s\n"), ((flags.machine_flags() & machine_flags::IS_BIOS_ROOT) ? _("Yes") : _("No")));
	util::stream_format(str, _("Support Save\t%1$s\n"), ((flags.machine_flags() & machine_flags::SUPPORTS_SAVE) ? _("Yes") : _("No")));
	util::stream_format(str, _("Screen Orientation\t%1$s\n"), ((flags.machine_flags() & ORIENTATION_SWAP_XY) ? _("Vertical") : _("Horizontal")));
	bool found = false;
	auto entries = rom_build_entries(driver->rom);
	for (const rom_entry &rom : entries)
		if (ROMENTRY_ISREGION(&rom) && ROMREGION_ISDISKDATA(&rom))
		{
			found = true;
			break;
		}
	util::stream_format(str, _("Requires CHD\t%1$s\n"), found ? _("Yes") : _("No"));

	// audit the game first to see if we're going to work
	if (ui().options().info_audit())
	{
		driver_enumerator enumerator(machine().options(), *driver);
		enumerator.next();
		media_auditor auditor(enumerator);
		media_auditor::summary summary = auditor.audit_media(AUDIT_VALIDATE_FAST);
		media_auditor::summary summary_samples = auditor.audit_samples();

		// if everything looks good, schedule the new driver
		if (summary == media_auditor::CORRECT || summary == media_auditor::BEST_AVAILABLE || summary == media_auditor::NONE_NEEDED)
			str << _("Roms Audit Pass\tOK\n");
		else
			str << _("Roms Audit Pass\tBAD\n");

		if (summary_samples == media_auditor::NONE_NEEDED)
			str << _("Samples Audit Pass\tNone Needed\n");
		else if (summary_samples == media_auditor::CORRECT || summary_samples == media_auditor::BEST_AVAILABLE)
			str << _("Samples Audit Pass\tOK\n");
		else
			str << _("Samples Audit Pass\tBAD\n");
	}
	else
		str << _("Roms Audit Pass\tDisabled\nSamples Audit Pass\tDisabled\n");

	buffer = str.str();
}

void menu_select_game::inkey_export()
{
	std::vector<game_driver const *> list;
	if (!m_search.empty())
	{
		for (int curitem = 0; m_searchlist[curitem]; ++curitem)
			list.push_back(m_searchlist[curitem]);
	}
	else
	{
		if (isfavorite())
		{
			// iterate over favorites
			for (auto & favmap : mame_machine_manager::instance()->favorite().m_list)
			{
				if (favmap.second.startempty == 1)
					list.push_back(favmap.second.driver);
				else
					return;
			}
		}
		else
		{
			list = m_displaylist;
		}
	}

	menu::stack_push<menu_export>(ui(), container(), std::move(list));
}

//-------------------------------------------------
//  save drivers infos to file
//-------------------------------------------------

void menu_select_game::init_sorted_list()
{
	if (!m_sortedlist.empty())
		return;

	// generate full list
	std::unordered_set<std::string> manufacturers, years;
	for (int x = 0; x < driver_list::total(); ++x)
	{
		game_driver const &driver(driver_list::driver(x));
		if (&driver != &GAME_NAME(___empty))
		{
			if (driver.flags & machine_flags::IS_BIOS_ROOT)
				m_isabios++;

			m_sortedlist.push_back(&driver);
			manufacturers.emplace(c_mnfct::getname(driver.manufacturer));
			years.emplace(driver.year);
		}
	}

	// sort manufacturers - years and driver
	for (auto it = manufacturers.begin(); manufacturers.end() != it; it = manufacturers.erase(it))
		c_mnfct::ui.emplace_back(*it);
	std::sort(c_mnfct::ui.begin(), c_mnfct::ui.end(), [] (std::string const &x, std::string const &y) { return 0 > core_stricmp(x.c_str(), y.c_str()); });
	for (auto it = years.begin(); years.end() != it; it = years.erase(it))
		c_year::ui.emplace_back(*it);
	std::stable_sort(c_year::ui.begin(), c_year::ui.end());
	std::stable_sort(m_sortedlist.begin(), m_sortedlist.end(), sorted_game_list);
}

//-------------------------------------------------
//  load drivers infos from file
//-------------------------------------------------

bool menu_select_game::load_available_machines()
{
	// try to load available drivers from file
	emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
	if (file.open(emulator_info::get_configname(), "_avail.ini") != osd_file::error::NONE)
		return false;

	std::string readbuf;
	char rbuf[MAX_CHAR_INFO];
	file.gets(rbuf, MAX_CHAR_INFO);
	file.gets(rbuf, MAX_CHAR_INFO);
	readbuf = chartrimcarriage(rbuf);
	std::string a_rev = string_format("%s%s", UI_VERSION_TAG, bare_build_version);

	// version not matching ? exit
	if (a_rev != readbuf)
	{
		file.close();
		return false;
	}

	file.gets(rbuf, MAX_CHAR_INFO);
	file.gets(rbuf, MAX_CHAR_INFO);
	file.gets(rbuf, MAX_CHAR_INFO);
	auto avsize = atoi(rbuf);
	file.gets(rbuf, MAX_CHAR_INFO);
	auto unavsize = atoi(rbuf);

	// load available list
	for (int x = 0; x < avsize; ++x)
	{
		file.gets(rbuf, MAX_CHAR_INFO);
		int find = atoi(rbuf);
		m_availsortedlist.push_back(&driver_list::driver(find));
	}

	// load unavailable list
	for (int x = 0; x < unavsize; ++x)
	{
		file.gets(rbuf, MAX_CHAR_INFO);
		int find = atoi(rbuf);
		m_unavailsortedlist.push_back(&driver_list::driver(find));
	}
	file.close();
	return true;
}

//-------------------------------------------------
//  load custom filters info from file
//-------------------------------------------------

void menu_select_game::load_custom_filters()
{
	emu_file file(ui().options().ui_path(), OPEN_FLAG_READ);
	if (file.open("custom_", emulator_info::get_configname(), "_filter.ini") == osd_file::error::NONE)
	{
		machine_filter::ptr flt(machine_filter::create(file));
		if (flt)
			main_filters::filters[flt->get_type()] = std::move(flt); // not emplace/insert - could replace bogus filter from ui.ini line
		file.close();
	}

}


//-------------------------------------------------
//  draw left box
//-------------------------------------------------

float menu_select_game::draw_left_panel(float x1, float y1, float x2, float y2)
{
	return menu_select_launch::draw_left_panel<machine_filter>(main_filters::actual, main_filters::filters, x1, y1, x2, y2);
}


//-------------------------------------------------
//  get selected software and/or driver
//-------------------------------------------------

void menu_select_game::get_selection(ui_software_info const *&software, game_driver const *&driver) const
{
	if (item[0].flags & FLAG_UI_FAVORITE) // TODO: work out why this doesn't use isfavorite()
	{
		software = reinterpret_cast<ui_software_info const *>(get_selection_ptr());
		driver = software ? software->driver : nullptr;
	}
	else
	{
		software = nullptr;
		driver = reinterpret_cast<game_driver const *>(get_selection_ptr());
	}
}

void menu_select_game::make_topbox_text(std::string &line0, std::string &line1, std::string &line2) const
{
	line0 = string_format(_("%1$s %2$s ( %3$d / %4$d machines (%5$d BIOS) )"),
			emulator_info::get_appname(),
			bare_build_version,
			visible_items,
			(driver_list::total() - 1),
			m_isabios);

	if (isfavorite())
	{
		line1.clear();
	}
	else
	{
		auto const it(main_filters::filters.find(main_filters::actual));
		char const *const filter((main_filters::filters.end() != it) ? it->second->filter_text() : nullptr);
		if (filter)
			line1 = string_format(_("%1$s: %2$s - Search: %3$s_"), it->second->display_name(), filter, m_search);
		else
			line1 = string_format(_("Search: %1$s_"), m_search);
	}

	line2.clear();
}


std::string menu_select_game::make_driver_description(game_driver const &driver) const
{
	// first line is game name
	return string_format(_("Romset: %1$-.100s"), driver.name);
}


std::string menu_select_game::make_software_description(ui_software_info const &software) const
{
	// first line is system
	return string_format(_("System: %1$-.100s"), software.driver->type.fullname());
}


void menu_select_game::filter_selected()
{
	if ((machine_filter::FIRST <= m_filter_highlight) && (machine_filter::LAST >= m_filter_highlight))
	{
		m_search.clear();
		auto it(main_filters::filters.find(machine_filter::type(m_filter_highlight)));
		if (main_filters::filters.end() == it)
			it = main_filters::filters.emplace(machine_filter::type(m_filter_highlight), machine_filter::create(machine_filter::type(m_filter_highlight))).first;
		it->second->show_ui(
				ui(),
				container(),
				[this] (machine_filter &filter)
				{
					machine_filter::type const new_type(filter.get_type());
					if (machine_filter::CUSTOM == new_type)
					{
						emu_file file(ui().options().ui_path(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
						if (file.open("custom_", emulator_info::get_configname(), "_filter.ini") == osd_file::error::NONE)
						{
							filter.save_ini(file, 0);
							file.close();
						}
					}
					main_filters::actual = new_type;
					reset(reset_options::SELECT_FIRST);
				});
	}
}


std::string menu_select_game::make_error_text(bool summary, media_auditor const &auditor)
{
	std::ostringstream str;
	str << _("The selected machine is missing one or more required ROM or CHD images. Please select a different machine.\n\n");
	if (summary)
	{
		auditor.summarize(nullptr, &str);
		str << "\n";
	}
	str << _("Press any key to continue.");
	return str.str();
}

} // namespace ui
