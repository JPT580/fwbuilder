/*

                          Firewall Builder

                 Copyright (C) 2008 NetCitadel, LLC

  Author:  Vadim Kurland     vadim@fwbuilder.org

  $Id$

  This program is free software which we release under the GNU General Public
  License. You may redistribute and/or modify this program under the terms
  of that license as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  To get a copy of the GNU General Public License, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "../../config.h"
#include "global.h"
#include "utils.h"
#include "utils_no_qt.h"

#include "compiler_lib/CompilerDriver.h"
#include "FirewallInstaller.h"
#include "instDialog.h"
#include "FWBSettings.h"
#include "FWWindow.h"
#include "SSHSession.h"
#include "SSHUnx.h"
#include "Configlet.h"
#include "UserWorkflow.h"

#include "fwbuilder/Resources.h"
#include "fwbuilder/FWObjectDatabase.h"
#include "fwbuilder/Firewall.h"
#include "fwbuilder/XMLTools.h"
#include "fwbuilder/Interface.h"
#include "fwbuilder/Management.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QString>
#include <QTextCodec>
#include <QTextStream>
#include <QTimer>
#include <QtDebug>
#include <QSettings>

#ifndef _WIN32
#  include <unistd.h>     // for access(2) and getdomainname
#endif

#include <errno.h>
#include <iostream>


using namespace std;
using namespace libfwbuilder;


bool FirewallInstaller::parseManifestLine(const QString &line,
                                          QString *local_file_name,
                                          QString *remote_file_name,
                                          bool *main_script)
{
    // generated IOS and PIX scripts use '!' as a comment which places
    // manifest marker at offset of 1 char from the beginning of the
    // line
    if (line.indexOf(MANIFEST_MARKER) == -1) return false;

    if (fwbdebug)
        qDebug("Manifest line: '%s'", line.toAscii().constData());

    QString workline = line.split(MANIFEST_MARKER)[1].trimmed();
    if (workline.startsWith("*"))
    {
        *main_script = true;
        workline = workline.remove(0, 1).trimmed();
    }
    QString local, remote;
    int i = 0;
    for(i=0; i<workline.size(); i++)
    {
        if (workline.at(i) == ' ' && workline.at(i-1) != '\\')
            break;
        else
            local.append(workline.at(i));
    }
    for(; i<workline.size(); i++)
    {
        if (workline.at(i) == ' ' && workline.at(i-1) != '\\')
            break;
        else
            remote.append(workline.at(i));
    }
    *local_file_name = fwcompiler::CompilerDriver::unescapeFileName(local);
    *remote_file_name = fwcompiler::CompilerDriver::unescapeFileName(remote);

    if (fwbdebug)
        qDebug("local_name: '%s'  remote_name: '%s'  main_script: %d",
               local_file_name->toAscii().constData(),
               remote_file_name->toAscii().constData(),
               *main_script);

    return true;
}

/*
 * FirewallInstaller::readManifest reads manifest from the generated
 * script and applies logic to decide the path and name of all files
 * that will be copied to the firewall.

Manifest format:

# files: * local_file_name remote_file_name

the '*' is optional and marks the "main" script, that is, the script
that should be executed on the firewall to activate policy. The part
'# files:' is manifest marker and must be reproduced just so. Parts
are separated by one or more spaces. Remote name is optional, if it is
missing, it is assumed to be equal to the local name. This provides
for backwards compatibility with previous versions where manifest did
not include remote name. Manifest can consit of multiple lines to
describe multiple files, although only one line can have '*'.

Installation process is controlled by several variables that the user
can change in the "advanced" dialog for the firewall platform:

Tab "Compiler":
 - output file name
 - script name on the firewall
 - for PF and ipfilter additionally .conf file name on the firewall

Tab "Installer":
 - directory on the firewall where script should be installed
 - command that installer should execute on the firewall

These variables have default values if input fields are left blank
in the dialog as follows:

output file name: the name of the firewall object, plus extension
".fw". For PF two files are generated: <firewall>.fw and
<firewall>.conf; for ipfilter files <firewall>.fw, <firewall>-ipf.conf
and <firewall>-nat.conf are generated.

script name on the firewall: the same as the output file name

directory on the firewall: "/etc"

command that installer executes to activate policy: installer runs
script <firewall>.fw

If user enters alternative name in the "script name on the firewall",
it is used when generated script is copied to the firewall. There are
two input fields in the dialogs for PF and ipf where user can enter
alternative name for the .fw script and .conf file. The name can be
relative or absolute path. If it is a relative path or just a file
name, it is treated as a file name in the directory specified by the
"directory on the firewall" input field in the "Installer" tab. If the
name is an absolute path, the directory entered in "directory on the
firewall..." input field is ignored.  If user entered alternative name
for the script on the firewall, the command that installer should
execute to activate it must be entered as well. If the alternative
name was entered as an absolute path, activation command should take
this into account and use the same absolute path. The command can
start with "sudo " if user account used to copy and activate policy is
not root.

*/

#include "CompilerDriver.h"

bool FirewallInstaller::readManifest(const QString &script, 
                                     QMap<QString, QString> *all_files)
{
    if (fwbdebug)
        qDebug("FirewallInstaller::readManifest");

    // Read generated config file (cnf->script), find manifest
    // and schedule copying of all files listed there.
    QFile cf(script);
    if (cf.open(QIODevice::ReadOnly ))
    {
        QTextStream stream(&cf);
        QString line;
        do
        {
            line = stream.readLine();
            if (line.isNull()) break;
            QString local_name;
            QString remote_name;
            bool main_script = false;
            if (parseManifestLine(line, &local_name, &remote_name, &main_script))
            {
                QFileInfo loc_file_info(local_name);
                if (!loc_file_info.isAbsolute())
                {
                    QFileInfo cnf_file_info(cnf->script);
                    if (cnf_file_info.isAbsolute())
                        local_name = cnf_file_info.dir().path() + "/" + local_name;
                }

                if (remote_name.isEmpty())
                {
                    QFileInfo loc_file_info(local_name);
                    remote_name = cnf->fwdir + "/" + loc_file_info.fileName();
                }

                // This is the manifest line with "*", it marks the main script
                // we should run.
                if (main_script)
                {
                    // Override directory variable if remote file name
                    // is an absolute path. This is used later to
                    // replace %FWDIR% macro

                    // Override fwbscript as well
                    // This is used later to replace %FWSCRIPT% macro
                    // getDestinationDir() returns corrected directory 
                    // depending on the user (root/regular) and temp install
                    // flag setting

                    QFileInfo rem_file_info(remote_name);
                    if (rem_file_info.isAbsolute())
                    {
                        cnf->fwdir = rem_file_info.dir().path();
                        cnf->remote_script = getDestinationDir(cnf->fwdir) +
                            rem_file_info.fileName();
                    } else
                    {
                        cnf->remote_script = getDestinationDir(cnf->fwdir) +
                            remote_name;
                    }
                }
                (*all_files)[local_name] = remote_name;
            }
        } while  (!line.isNull());
        cf.close();

        if (cnf->remote_script.isEmpty())
        {
            // manifest did not include line with '*'
            cnf->remote_script = getDestinationDir(cnf->fwdir) + cnf->script;
        }

        // Now that we have found the main script and know its
        // location (in case user provided absolute path for the
        // remote file name variable) we can update remote path for
        // all files
        QMap<QString, QString>::iterator it;
        for (it=all_files->begin(); it!=all_files->end(); ++it)
        {
            QString local_name = it.key();
            QString remote_name = it.value();
            QFileInfo rem_file_info(remote_name);
            if (rem_file_info.isAbsolute())
                (*all_files)[local_name] =
                    getDestinationDir(rem_file_info.dir().path()) + rem_file_info.fileName();
            else
                (*all_files)[local_name] =
                    getDestinationDir(cnf->fwdir) + remote_name;
        }

        // make sure remote_name has '/' as a separator. If the program
        // runs on windows, QFileInfo may return path with native
        // separators '\'
        cnf->remote_script = QDir::fromNativeSeparators(cnf->remote_script);
        for (it=all_files->begin(); it!=all_files->end(); ++it)
        {
            QString local_name = it.key();
            (*all_files)[local_name] = QDir::fromNativeSeparators(it.value());
        }
        return true;
    } else
    {
        QMessageBox::critical(
            inst_dlg, "Firewall Builder",
            tr("Generated script file %1 not found.").arg(script),
            tr("&Continue") );
        return false;
    }
}

bool FirewallInstaller::packInstallJobsList(Firewall*)
{
    return false;
}

void FirewallInstaller::packSSHArgs(QStringList &args)
{
    QString ssh = st->getSSHPath();

    QStringList ssh_argv;
    parseCommandLine(ssh, ssh_argv);

#ifdef _WIN32
    args += ssh_argv;

    /*
     * putty ignores protocol and port specified in the session file if
     * command line option -ssh is given.
     *
     * On the other hand,the sign of session usage is an empty user name,
     * so we can check for that. If user name is empty, then putty will
     * use current Windows account name to log in to the firewall and this
     * is unlikely to work anyway. This seems to be a decent workaround.
     */
    if (ssh.contains("plink.exe", Qt::CaseInsensitive))
    {
        args.push_back("-ssh");
        args.push_back("-t");

        if (cnf->putty_session.isEmpty())
        {
            args.push_back("-load");
            args.push_back("fwb_session_with_keepalive");
        }


        if (!cnf->pwd.isEmpty())
        {
            args.push_back("-pw");
            args.push_back(cnf->pwd);
        }
    }

#else

    args.push_back(argv0.c_str());

    args.push_back("-X");   // fwbuilder works as ssh wrapper

    args += ssh_argv;

    args.push_back("-o");
    args.push_back(QString("ServerAliveInterval=%1").arg(st->getSSHTimeout()));

    args.push_back("-t");
    args.push_back("-t");

#endif

    if (!cnf->sshArgs.isEmpty())
        args += cnf->sshArgs.split(" ", QString::SkipEmptyParts);

    if (cnf->verbose) args.push_back("-v");

    if (!cnf->user.isEmpty())
    {
        args.push_back("-l");
        args.push_back(cnf->user);
    }

    if (!cnf->putty_session.isEmpty())
    {
        args.push_back(cnf->putty_session);
    } else
    {
        args.push_back(cnf->maddr);
    }
}

void FirewallInstaller::packSCPArgs(const QString &local_name,
                                    const QString &remote_name, 
                                    QStringList &args)
{
    QString file_with_path = getFullPath(local_name);
    QString scp = st->getSCPPath();

    QStringList scp_argv;
    parseCommandLine(scp, scp_argv);

    QString mgmt_addr = cnf->maddr;

    /*
     * bug #2618686 "built-in installer can not handle ipv6 management
     * address". If cnf->maddr is ipv6 address, it needs to be placed in
     * [ ] for scp (otherwise scp interprets ':' as a separator between
     * host name and port number).
     * Note that this is only necessary for scp; ssh takes ipv6 addresses
     * without [ ] just fine.
     */

    try
    {
        InetAddr addr(AF_INET6, cnf->maddr.toAscii().constData());
        if (fwbdebug)
            qDebug("SCP will talk to the firewall using address %s ( %s )",
                   cnf->maddr.toAscii().constData(),
                   addr.toString().c_str());
        /*
         * It looks like if cnf->maddr is a host name, then InetAddr
         * does not fail but just creates address '::'.
         * InetAddr throws exception if it is given ipv4 address.
         * Only add [ ] if this is legitimate ipv6 address (not '::')
         */
        if (!addr.isAny())
            mgmt_addr = '[' + cnf->maddr + ']';

    } catch(FWException &ex)
    {
        // Assume cnf->maddr is ipv4 or host name
        ;
    }


#ifdef _WIN32
    args += scp_argv;
 
    /*
     * See #1832. plink.exe and pscp.exe do not try to interpret target
     * host name as session name if they see "-load session_name" on the
     * command line. This means if user wants to use session, we can not
     * load session fwb_session_with_keepalive.
     */
    if (scp.contains("pscp.exe", Qt::CaseInsensitive))
    {
        if (cnf->putty_session.isEmpty())
        {
            args.push_back("-load");
            args.push_back("fwb_session_with_keepalive");
        }

        if (!cnf->pwd.isEmpty())
        {
            args.push_back("-pw");
            args.push_back(cnf->pwd);
        }
    }

#else

    args.push_back(argv0.c_str());
    args.push_back("-Y");   // fwbuilder works as scp wrapper

    args += scp_argv;

    args.push_back("-o");
    // "3" here is the default value of ServerAliveCountMax parameter
    // This way, overall timeout will be the same for ssh and scp
    args.push_back(QString("ConnectTimeout=%1").arg(st->getSSHTimeout() * 3));

#endif

    if (!cnf->scpArgs.isEmpty())
        args += cnf->scpArgs.split(" ", QString::SkipEmptyParts);

    args.push_back("-q");

    args.push_back(file_with_path);

    /*
     * bug #2618772: "test install" option does not work.  To fix, I
     * put macro for the temp directory in in res/os/host_os.xml XML
     * elements root/test/copy reg_user/test/copy. That macro
     * is read and processed by getDestinationDir()
     *
     * Also note that pscp.exe supports "-l user" command line arg,
     * but unix scp does not. Both support user@target format though
     * and it works with sessions for pscp.exe
     */

    QString user_spec;
    if (!cnf->user.isEmpty()) user_spec = cnf->user + "@";

    QString target;

    if (!cnf->putty_session.isEmpty())
        target = QString("%1%2:%3").arg(user_spec)
            .arg(cnf->putty_session).arg(fwcompiler::CompilerDriver::escapeFileName(remote_name));
    else
        target = QString("%1%2:%3").arg(user_spec)
            .arg(mgmt_addr).arg(fwcompiler::CompilerDriver::escapeFileName(remote_name));

    args.push_back(target);

/*
    if (!cnf->putty_session.isEmpty())
    {
        args.push_back(cnf->putty_session + ":" +
                       fwcompiler::CompilerDriver::escapeFileName(remote_name));
    } else
    {
        if (!cnf->user.isEmpty())
            args.push_back(cnf->user + "@" + mgmt_addr + ":" +
                           fwcompiler::CompilerDriver::escapeFileName(remote_name));
        else
            args.push_back(mgmt_addr + ":" +
                           fwcompiler::CompilerDriver::escapeFileName(remote_name));
    }
*/
}

/*
 * take next job from job_list and execute it.
 *
 * Note that this slot is called when SSHSession emits signal
 * sessionFinished.  This happens outside of control of the
 * instDialog. If user clicked Cancel or Finish button (even though
 * Finish should not be active, but still), runJobs() should not
 * continue. Check for this condition using instDialog::isFinished()
 *
 */
void FirewallInstaller::runJobs()
{
    if (fwbdebug) qDebug("FirewallInstaller::runJobs");

    if (inst_dlg->isFinished()) return;

    if (job_list.size()==0)
    {
        if (fwbdebug) qDebug("FirewallInstaller::runJobs:  job list is empty");
        QTimer::singleShot( 1000, inst_dlg, SLOT(mainLoopInstall()));
        return;
    }

    instJob current_job = job_list.front();
    job_list.pop_front();

    wfl->registerFlag(UserWorkflow::INSTALL_RAN, true);

    switch (current_job.job) 
    {
    case COPY_FILE:
        copyFile(current_job.argument1, current_job.argument2);
        break;

    case EXECUTE_COMMAND:
        executeCommand(current_job.argument1);
        break;

    case ACTIVATE_POLICY:
        activatePolicy(current_job.argument1, current_job.argument2);
        break;

    case RUN_EXTERNAL_SCRIPT:
        executeExternalInstallScript(current_job.argument1, current_job.argument2);
        break;
    }
}

/*
 * copyFile starts background process and returns. Process object
 * emits signal finished() which will be connected to slot
 * commandFinished(). This slot checks termination status of the process
 * and if it was successfull, it schedules call to runJobs()
 */
void FirewallInstaller::copyFile(const QString &local_name, const QString &remote_name)
{
    //QString platform = cnf->fwobj->getStr("platform").c_str();

//    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("latin1"));

    QStringList args;
    packSCPArgs(local_name, remote_name, args);

    inst_dlg->addToLog( tr("Copying %1 -> %2:%3\n")
                        .arg(QString::fromUtf8(local_name.toAscii().constData()))
                        .arg(cnf->maddr)
                        .arg(QString::fromUtf8(remote_name.toAscii().constData())));

    if (cnf->verbose) inst_dlg->displayCommand(args);
    qApp->processEvents();

    // Need session for scp copy because we need to enter password
    runSSHSession( new SSHUnx(inst_dlg,
                              cnf->fwobj->getName().c_str(),
                              args,
                              cnf->pwd,
                              "",
                              list<string>()), true );
}

void FirewallInstaller::executeExternalInstallScript(const QString &command,
                                                     const QString &script_args)
{
    FWObjectDatabase *db = cnf->fwobj->getRoot();
    assert(db);

    QString wdir = getFileDir( mw->getRCS()->getFileName() );
    QStringList args;
    //args.push_back(command.trimmed());

    args += script_args.trimmed().split(" ", QString::SkipEmptyParts);

    args.push_back("-f");
    args.push_back(db->getFileName().c_str());

    if (wdir!="")
    {
        args.push_back("-d");
        args.push_back(wdir);
    }
    args.push_back(cnf->fwobj->getName().c_str());

    if (cnf->verbose) inst_dlg->displayCommand(args);
    qApp->processEvents();

    inst_dlg->setUpProcessToInstall();
    if (!inst_dlg->executeCommand(command.trimmed(), args))
        QTimer::singleShot( 0, inst_dlg, SLOT(mainLoopInstall()));
}

void FirewallInstaller::executeCommand(const QString &cmd)
{
    QStringList args;
    packSSHArgs(args);
    args.push_back( cmd );
    if (cnf->verbose) inst_dlg->displayCommand(args);
    qApp->processEvents();

    QString path = args.at(0);
    args.pop_front();

    inst_dlg->setUpProcessToInstall();
    if (!inst_dlg->executeCommand(path, args))
        QTimer::singleShot( 0, inst_dlg, SLOT(mainLoopInstall()));
}

// ************************************************************************

void FirewallInstaller::activatePolicy(const QString&,  const QString&)
{
    QTimer::singleShot( 0, this, SLOT(runJobs()));
}    

/*
 * parameter intermediate: if true, then this session is part of the
 * set required to complete install on single firewall, such as when
 * we need to copy several files and then activate policy. If this
 * parameter is false, the session is final and installer terminates
 * when it finishes. This only applies to when session finishes
 * successfully. If session finishes with an error, we always
 * terminate installer.
 */
void FirewallInstaller::runSSHSession(SSHSession *s, bool intermediate)
{
    if (fwbdebug) qDebug("FirewallInstaller::runSSHSession()");

    session = s;

    session->setOptions(cnf);
    session->setFWBPrompt(fwb_prompt);

    connect(session,SIGNAL(printStdout_sign(const QString&)),
            inst_dlg, SLOT(addToLog(const QString&)));

    if (intermediate)
        connect(session,SIGNAL(sessionFinished_sign()),
                this, SLOT(runJobs()));
    else
        connect(session,SIGNAL(sessionFinished_sign()),
                inst_dlg, SLOT(installerSuccess()));

    connect(session,SIGNAL(sessionFatalError_sign()),
            inst_dlg, SLOT(installerError()));

    connect(session,SIGNAL(updateProgressBar_sign(int,bool)),
            inst_dlg, SLOT(updateProgressBar(int,bool)));

    session->startSession();
}

QString FirewallInstaller::getFullPath(const QString &file )
{
    if (QDir::isRelativePath(file)) return cnf->wdir + "/" + file;
    else return file;
}


/*
 * This method builds and returns activation command
 * This method is used for all firewall platforms but PIX
 */
QString FirewallInstaller::getActivationCmd()
{
    if (!cnf->activationCmd.isEmpty())
    {
        return cnf->activationCmd;
    }

    QString configlet_name = "installer_commands_";
    if (cnf->user=="root") configlet_name += "root";
    else                   configlet_name += "reg_user";

    string host_os = cnf->fwobj->getStr("host_OS");
    string os_family = Resources::os_res[host_os]->
        getResourceStr("/FWBuilderResources/Target/family");

    // installer configlets should be different for each OS, but if
    // some OS can use the same script, it will be placed in the file
    // under os_family name. For example:
    // for linksys/sveasoft configlet is in src/res/configlets/sveasoft
    // but since linux24 and openwrt can use the same script, it is
    // located in src/res/configlets/linux24 (openwrt.xml file defines
    // family as "linux24")
    Configlet configlet(host_os, os_family, configlet_name);
    configlet.removeComments();
    configlet.collapseEmptyStrings(true);
    configlet.setVariable("test",  cnf->testRun);
    configlet.setVariable("run", ! cnf->testRun);
    configlet.setVariable("with_rollback",   cnf->rollback);
    configlet.setVariable("no_rollback",   ! cnf->rollback);
    configlet.setVariable("with_compression",  cnf->compressScript);
    configlet.setVariable("no_compression",  ! cnf->compressScript);

    replaceMacrosInCommand(&configlet);

    return configlet.expand().trimmed();
}

void FirewallInstaller::replaceMacrosInCommand(Configlet *conf)
{
/* replace macros in activation commands:
 *
 * {{$fwbpromp}}   -- "magic" prompt that installer uses to detect when it is logged in
 * {{$fwdir}}      -- directory on the firewall
 * {{$fwscript}}   -- script name on the firewall
 * {{$rbtimeout}}  -- rollbak timeout
 */

/*
 * TODO: it does not make sense to split remote_script and then
 * reassemble it again from the file name and cnf.fwdir. We should set
 * variable $remote_script and use it in the configlets instead, but
 * keep $fwbscript and $fwdir for backwards compatibility
 */

/*
 * remote_script is a full path, which in case of Cisco can be
 * something like "flash:file.fw". This means we have a problem with
 * QFileInfo that interprets it as path:filename on Window or just
 * file name with no directory path on Unix. As the result, fwbscript
 * becomes just "file.fw" on Windows and stays "flash:file.fw" on
 * Unix.
 */

    QString fwbscript = fwcompiler::CompilerDriver::escapeFileName(
        QFileInfo(cnf->remote_script).fileName());

    if (fwbscript.indexOf(":")!=-1) fwbscript = fwbscript.section(':', 1, 1);

    if (fwbdebug)
    {
        qDebug() << "Macro substitutions:";
        qDebug() << "  $fwdir=" << cnf->fwdir;
        qDebug() << "  cnf->script=" << cnf->script;
        qDebug() << "  cnf->remote_script=" << cnf->remote_script;
        qDebug() << "  $fwscript=" << fwbscript;
    }

    conf->setVariable("fwbprompt", fwb_prompt);
    conf->setVariable("fwdir", cnf->fwdir);
    conf->setVariable("fwscript", fwbscript);

    conf->setVariable("rbtimeout", cnf->rollbackTime);
    conf->setVariable("rbtimeout_sec", cnf->rollbackTime * 60);
}


/*
 * Takes destination directory defined in the configlet (or XML
 * resource file) and substitutes {{$fwbdir}} macro with
 * @fwdir. Returned directory path always ends with separator ("/")
 *
 * Main purpose of this method is to get the right directory depending
 * on the setting of the "test install" option. In case of test
 * install we copy all files into a different directory and run them
 * from there. The directory is defined in the resource (or configlet)
 * file.
 */
QString FirewallInstaller::getDestinationDir(const QString &fwdir)
{
    QString dir = "";

    if (cnf->testRun)
    {
        string optpath = "activation/fwdir_test/";
        dir = Resources::getTargetOptionStr(cnf->fwobj->getStr("host_OS"),
                                            optpath).c_str();
        // need to trim dir because it picks up '\n' and possibly spaces
        // from XML element body text formatting
        dir = dir.trimmed();
    }

    if (fwbdebug)
        qDebug() << "FirewallInstaller::getDestinationDir:  "
                 << "destination directory=" << dir
                 << "cnf->fwdir=" << cnf->fwdir;
    
    // dir can contain macro %FWDIR% which should be replaced with cnf->fwdir
    // empty dir is equivalent to just the value of cnf->fwdir

    if (!dir.isEmpty())
        dir.replace("{{$fwdir}}", fwdir);
    else
        dir = fwdir;

    if (!dir.endsWith(QDir::separator())) return dir + "/";
    return dir;
}

QString FirewallInstaller::getGeneratedFileFullPath(Firewall *fw)
{
/* bug #1617501: "Install fails after compile". The "output file"
 * setting that user enters in the "Compiler" tab of fw advanced
 * dialog can be either local or absolute path. 
 */
    FWOptions  *fwopt = fw->getOptionsObject();
    QString generated_file;
    QString ofname = QString::fromUtf8(fwopt->getStr("output_file").c_str());
    if (!ofname.isEmpty())
    {
        QFileInfo fi(ofname);
        if (fi.suffix() != "fw") generated_file = ofname + ".fw";
        else generated_file = ofname;
    } else
        generated_file = QString::fromUtf8(fw->getName().c_str()) + ".fw";

    QFileInfo gen_file_info(generated_file);
    if (!gen_file_info.isAbsolute())
    {
        QFileInfo fwb_file_info = QFileInfo(mw->getRCS()->getFileName());
        generated_file = fwb_file_info.dir().path() + "/" + generated_file;
    }
    return QDir::toNativeSeparators(generated_file);
}
 
void FirewallInstaller::terminate()
{
    if (session != NULL)
    {
        session->terminate();
    }
}
