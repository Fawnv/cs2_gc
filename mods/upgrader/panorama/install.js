try {
            CsgcUpgrader.Install($.GetContextPanel(), _m_elInventoryMain, NavigateToTab);
        }
        catch (e) {
            $.Msg('[csgc upgrader] ' + e + '\n' + e.stack + '\n');
        }
        