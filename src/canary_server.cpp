/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"

#include "canary_server.hpp"

#include "declarations.hpp"
#include "creatures/players/grouping/familiars.hpp"
#include "creatures/players/storages/storages.hpp"
#include "database/databasemanager.hpp"
#include "game/game.hpp"
#include "game/zones/zone.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "game/scheduling/events_scheduler.hpp"
#include "io/iomarket.hpp"
#include "lib/thread/thread_pool.hpp"
#include "lua/creature/events.hpp"
#include "lua/modules/modules.hpp"
#include "lua/scripts/lua_environment.hpp"
#include "lua/scripts/scripts.hpp"
#include "server/network/protocol/protocollogin.hpp"
#include "server/network/protocol/protocolstatus.hpp"
#include "server/network/webhook/webhook.hpp"
#include "io/ioprey.hpp"
#include "io/io_bosstiary.hpp"

#include "core.hpp"

CanaryServer::CanaryServer(
	Logger &logger,
	RSA &rsa,
	ServiceManager &serviceManager
) :
	logger(logger),
	rsa(rsa),
	serviceManager(serviceManager) {
	logInfos();
	toggleForceCloseButton();
	g_game().setGameState(GAME_STATE_STARTUP);
	std::set_new_handler(badAllocationHandler);
	srand(static_cast<unsigned int>(OTSYS_TIME()));

	g_dispatcher().init();

#ifdef _WIN32
	SetConsoleTitleA(ProtocolStatus::SERVER_NAME.c_str());
#endif
}

int CanaryServer::run() {
	g_dispatcher().addEvent(
		[this] {
			try {
				loadConfigLua();

				logger.info("Server protocol: {}.{}{}", CLIENT_VERSION_UPPER, CLIENT_VERSION_LOWER, g_configManager().getBoolean(OLD_PROTOCOL, __FUNCTION__) ? " and 10x allowed!" : "");
				metrics::Options metricsOptions;
				metricsOptions.enablePrometheusExporter = g_configManager().getBoolean(METRICS_ENABLE_PROMETHEUS, __FUNCTION__);
				if (metricsOptions.enablePrometheusExporter) {
					metricsOptions.prometheusOptions.url = g_configManager().getString(METRICS_PROMETHEUS_ADDRESS, __FUNCTION__);
				}
				metricsOptions.enableOStreamExporter = g_configManager().getBoolean(METRICS_ENABLE_OSTREAM, __FUNCTION__);
				if (metricsOptions.enableOStreamExporter) {
					metricsOptions.ostreamOptions.export_interval_millis = std::chrono::milliseconds(g_configManager().getNumber(METRICS_OSTREAM_INTERVAL, __FUNCTION__));
				}
				g_metrics().init(metricsOptions);

				rsa.start();
				initializeDatabase();
				loadModules();
				setWorldType();
				loadMaps();

				logger.info("Initializing gamestate...");
				g_game().setGameState(GAME_STATE_INIT);

				setupHousesRent();
				g_game().transferHouseItemsToDepot();

				IOMarket::checkExpiredOffers();
				IOMarket::getInstance().updateStatistics();

				logger.info("Loaded all modules, server starting up...");

#ifndef _WIN32
				if (getuid() == 0 || geteuid() == 0) {
					logger.warn("{} has been executed as root user, "
								"please consider running it as a normal user",
								ProtocolStatus::SERVER_NAME);
				}
#endif

				g_game().start(&serviceManager);
				g_game().setGameState(GAME_STATE_NORMAL);
				if (g_configManager().getBoolean(TOGGLE_MAINTAIN_MODE, __FUNCTION__)) {
					g_game().setGameState(GAME_STATE_CLOSED);
					g_logger().warn("Initialized in maintain mode!");
					g_webhook().sendMessage(":yellow_square: Server is now **online** _(access restricted to staff)_");
				} else {
					g_game().setGameState(GAME_STATE_NORMAL);
					g_webhook().sendMessage(":green_circle: Server is now **online**");
				}

				loaderStatus = LoaderStatus::LOADED;
			} catch (FailedToInitializeCanary &err) {
				loaderStatus = LoaderStatus::FAILED;
				logger.error(err.what());

				logger.error("The program will close after pressing the enter key...");

				if (isatty(STDIN_FILENO)) {
					getchar();
				}
			}

			loaderStatus.notify_one();
		},
		"CanaryServer::run"
	);

	loaderStatus.wait(LoaderStatus::LOADING);

	if (loaderStatus == LoaderStatus::FAILED || !serviceManager.is_running()) {
		logger.error("No services running. The server is NOT online!");
		shutdown();
		return EXIT_FAILURE;
	}

	logger.info("{} {}", g_configManager().getString(SERVER_NAME, __FUNCTION__), "server online!");

	serviceManager.run();

	shutdown();
	return EXIT_SUCCESS;
}

void CanaryServer::setWorldType() {
	const std::string worldType = asLowerCaseString(g_configManager().getString(WORLD_TYPE, __FUNCTION__));
	if (worldType == "pvp") {
		g_game().setWorldType(WORLD_TYPE_PVP);
	} else if (worldType == "no-pvp") {
		g_game().setWorldType(WORLD_TYPE_NO_PVP);
	} else if (worldType == "pvp-enforced") {
		g_game().setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		throw FailedToInitializeCanary(
			fmt::format(
				"Unknown world type: {}, valid world types are: pvp, no-pvp and pvp-enforced",
				g_configManager().getString(WORLD_TYPE, __FUNCTION__)
			)
		);
	}

	logger.debug("World type set as {}", asUpperCaseString(worldType));
}

void CanaryServer::loadMaps() const {
	try {
		g_game().loadMainMap(g_configManager().getString(MAP_NAME, __FUNCTION__));

		// If "mapCustomEnabled" is true on config.lua, then load the custom map
		if (g_configManager().getBoolean(TOGGLE_MAP_CUSTOM, __FUNCTION__)) {
			g_game().loadCustomMaps(g_configManager().getString(DATA_DIRECTORY, __FUNCTION__) + "/world/custom/");
		}
		Zone::refreshAll();
	} catch (const std::exception &err) {
		throw FailedToInitializeCanary(err.what());
	}
}

void CanaryServer::setupHousesRent() {
	RentPeriod_t rentPeriod;
	std::string strRentPeriod = asLowerCaseString(g_configManager().getString(HOUSE_RENT_PERIOD, __FUNCTION__));

	if (strRentPeriod == "yearly") {
		rentPeriod = RENTPERIOD_YEARLY;
	} else if (strRentPeriod == "weekly") {
		rentPeriod = RENTPERIOD_WEEKLY;
	} else if (strRentPeriod == "monthly") {
		rentPeriod = RENTPERIOD_MONTHLY;
	} else if (strRentPeriod == "daily") {
		rentPeriod = RENTPERIOD_DAILY;
	} else {
		rentPeriod = RENTPERIOD_NEVER;
	}

	g_game().map.houses.payHouses(rentPeriod);
}

void CanaryServer::logInfos() {
#if defined(GIT_RETRIEVED_STATE) && GIT_RETRIEVED_STATE
	logger.debug("{} - Version [{}] dated [{}]", ProtocolStatus::SERVER_NAME, SERVER_RELEASE_VERSION, GIT_COMMIT_DATE_ISO8601);
	#if GIT_IS_DIRTY
	logger.debug("DIRTY - NOT OFFICIAL RELEASE");
	#endif
#else
	logger.info("{} - Version {}", ProtocolStatus::SERVER_NAME, SERVER_RELEASE_VERSION);
#endif

	logger.debug("Compiled with {}, on {} {}, for platform {}\n", getCompiler(), __DATE__, __TIME__, getPlatform());

#if defined(LUAJIT_VERSION)
	logger.debug("Linked with {} for Lua support", LUAJIT_VERSION);
#endif

	logger.info("A server developed by: {}", ProtocolStatus::SERVER_DEVELOPERS);
	logger.info("Visit our website for updates, support, and resources: "
				"https://docs.opentibiabr.com/");
}

/**
 *It is preferable to keep the close button off as it closes the server without saving (this can cause the player to lose items from houses and others informations, since windows automatically closes the process in five seconds, when forcing the close)
 * Choose to use "CTROL + C" or "CTROL + BREAK" for security close
 * To activate/deactivate window;
 * \param MF_GRAYED Disable the "x" (force close) button
 * \param MF_ENABLED Enable the "x" (force close) button
 */
void CanaryServer::toggleForceCloseButton() {
#ifdef OS_WINDOWS
	const HWND hwnd = GetConsoleWindow();
	const HMENU hmenu = GetSystemMenu(hwnd, FALSE);
	EnableMenuItem(hmenu, SC_CLOSE, MF_GRAYED);
#endif
}

void CanaryServer::badAllocationHandler() {
	// Use functions that only use stack allocation
	g_logger().error("Allocation failed, server out of memory, "
					 "decrease the size of your map or compile in 64 bits mode");

	if (isatty(STDIN_FILENO)) {
		getchar();
	}

	shutdown();
	exit(-1);
}

std::string CanaryServer::getPlatform() {
#if defined(__amd64__) || defined(_M_X64)
	return "x64";
#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
	return "x86";
#elif defined(__arm__)
	return "ARM";
#else
	return "unknown";
#endif
}

std::string CanaryServer::getCompiler() {
	std::string compiler;
#if defined(__clang__)
	return compiler = fmt::format("Clang++ {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_VER)
	return compiler = fmt::format("Microsoft Visual Studio {}", _MSC_VER);
#elif defined(__GNUC__)
	return compiler = fmt::format("G++ {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
	return compiler = "unknown";
#endif
}

void CanaryServer::loadConfigLua() {
	std::string configName = "config.lua";
	// Check if config or config.dist exist
	std::ifstream c_test("./" + configName);
	if (!c_test.is_open()) {
		std::ifstream config_lua_dist(configName + ".dist");
		if (config_lua_dist.is_open()) {
			logger.info("Copying {}.dist to {}", configName, configName);
			std::ofstream config_lua(configName);
			config_lua << config_lua_dist.rdbuf();
			config_lua.close();
			config_lua_dist.close();
		}
	} else {
		c_test.close();
	}

	g_configManager().setConfigFileLua(configName);

	modulesLoadHelper(g_configManager().load(), g_configManager().getConfigFileLua());

#ifdef _WIN32
	const std::string &defaultPriority = g_configManager().getString(DEFAULT_PRIORITY, __FUNCTION__);
	if (strcasecmp(defaultPriority.c_str(), "high") == 0) {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	} else if (strcasecmp(defaultPriority.c_str(), "above-normal") == 0) {
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	}
#endif
}

void CanaryServer::initializeDatabase() {
	logger.info("Establishing database connection... ");
	if (!Database::getInstance().connect()) {
		throw FailedToInitializeCanary("Failed to connect to database!");
	}
	logger.debug("MySQL Version: {}", Database::getClientVersion());

	logger.debug("Running database manager...");
	if (!DatabaseManager::isDatabaseSetup()) {
		throw FailedToInitializeCanary(fmt::format(
			"The database you have specified in {} is empty, please import the schema.sql to your database.",
			g_configManager().getConfigFileLua()
		));
	}

	DatabaseManager::updateDatabase();

	if (g_configManager().getBoolean(OPTIMIZE_DATABASE, __FUNCTION__)
		&& !DatabaseManager::optimizeTables()) {
		logger.debug("No tables were optimized");
	}
}

void CanaryServer::loadModules() {
	// If "USE_ANY_DATAPACK_FOLDER" is set to true then you can choose any datapack folder for your server
	const auto useAnyDatapack = g_configManager().getBoolean(USE_ANY_DATAPACK_FOLDER, __FUNCTION__);
	auto datapackName = g_configManager().getString(DATA_DIRECTORY, __FUNCTION__);
	if (!useAnyDatapack && (datapackName != "data-canary" && datapackName != "data-otservbr-global" || datapackName != "data-otservbr-global" && datapackName != "data-canary")) {
		throw FailedToInitializeCanary(fmt::format(
			"The datapack folder name '{}' is wrong, please select valid "
			"datapack name 'data-canary' or 'data-otservbr-global "
			"or enable in config.lua to use any datapack folder",
			datapackName
		));
	}

	logger.debug("Initializing lua environment...");
	if (!g_luaEnvironment().getLuaState()) {
		g_luaEnvironment().initState();
	}

	auto coreFolder = g_configManager().getString(CORE_DIRECTORY, __FUNCTION__);
	// Load items dependencies
	modulesLoadHelper((g_game().loadAppearanceProtobuf(coreFolder + "/items/appearances.dat") == ERROR_NONE), "appearances.dat");
	modulesLoadHelper(Item::items.loadFromXml(), "items.xml");

	const auto datapackFolder = g_configManager().getString(DATA_DIRECTORY, __FUNCTION__);
	logger.debug("Loading core scripts on folder: {}/", coreFolder);
	// Load first core Lua libs
	modulesLoadHelper((g_luaEnvironment().loadFile(coreFolder + "/core.lua", "core.lua") == 0), "core.lua");
	modulesLoadHelper(g_scripts().loadScripts(coreFolder + "/scripts", false, false), "/data/scripts");

	// Second XML scripts
	modulesLoadHelper(g_vocations().loadFromXml(), "XML/vocations.xml");
	modulesLoadHelper(g_eventsScheduler().loadScheduleEventFromXml(), "XML/events.xml");
	modulesLoadHelper(Outfits::getInstance().loadFromXml(), "XML/outfits.xml");
	modulesLoadHelper(Familiars::getInstance().loadFromXml(), "XML/familiars.xml");
	modulesLoadHelper(g_imbuements().loadFromXml(), "XML/imbuements.xml");
	modulesLoadHelper(g_storages().loadFromXML(), "XML/storages.xml");
	modulesLoadHelper(g_modules().loadFromXml(), "modules/modules.xml");
	modulesLoadHelper(g_events().loadFromXml(), "events/events.xml");
	modulesLoadHelper((g_npcs().load(true, false)), "npclib");

	logger.debug("Loading datapack scripts on folder: {}/", datapackName);
	modulesLoadHelper(g_scripts().loadScripts(datapackFolder + "/scripts/lib", true, false), datapackFolder + "/scripts/libs");
	// Load scripts
	modulesLoadHelper(g_scripts().loadScripts(datapackFolder + "/scripts", false, false), datapackFolder + "/scripts");
	// Load monsters
	modulesLoadHelper(g_scripts().loadScripts(datapackFolder + "/monster", false, false), datapackFolder + "/monster");
	modulesLoadHelper((g_npcs().load(false, true)), "npc");

	g_game().loadBoostedCreature();
	g_ioBosstiary().loadBoostedBoss();
	g_ioprey().initializeTaskHuntOptions();
}

void CanaryServer::modulesLoadHelper(bool loaded, std::string moduleName) {
	logger.debug("Loading {}", moduleName);
	if (!loaded) {
		throw FailedToInitializeCanary(fmt::format("Cannot load: {}", moduleName));
	}
}

void CanaryServer::shutdown() {
	g_dispatcher().shutdown();
	g_metrics().shutdown();
	inject<ThreadPool>().shutdown();
}
