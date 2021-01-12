/*
 * Copyright (c) 2006 Sean C. Rhea (srhea@srhea.net)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "Context.h"
#include "Athlete.h"
#include "MainWindow.h"

#include "ArchiveFile.h"

#include "RideItem.h"
#include "RideFile.h"
#include "RideImportWizard.h"
#include "RideCache.h"

#include "RideAutoImportConfig.h"
#include "HelpWhatsThis.h"
#include "Settings.h"
#include "Colors.h"
#include "Units.h"

#include "GcRideFile.h"
#include "JsonRideFile.h"
#include "TcxRideFile.h" // for opening multi-ride file
#include "DataProcessor.h"
#include "RideMetadata.h" // for linked defaults processing

#include <QDebug>
#include <QWaitCondition>
#include <QMessageBox>

ImportFile::ImportFile() :
    name(""), copyOnImport(true) {
}

ImportFile::ImportFile(const QString& fileName) :
    name(fileName), copyOnImport(true) {}

ImportFile::ImportFile(const QString& fileName, bool copyFileOnImport) :
    name(fileName), copyOnImport(copyFileOnImport) {}


enum WizardTable {
    FILENAME_COLUMN = 0,
    DATE_COLUMN,
    TIME_COLUMN,
    DURATION_COLUMN,
    DISTANCE_COLUMN,
    COPY_ON_IMPORT_COLUMN,
    STATUS_COLUMN,
    TOTAL_NUM_COLUMNS,
};

enum WizardTable1 {
    DIRECTORY_RULE_COLUMN = 0,
    IMPORT_RULE_COLUMN,
    COPY_ON_IMPORT_RULE_COLUMN,
    DIRECTORY_STATUS_RULE_COLUMN,
    TOTAL_NUM_RULE_COLUMNS,
};

// drag and drop passes urls ... convert to a list of files and call main constructor
RideImportWizard::RideImportWizard(QList<QUrl> *urls, Context *context, QWidget *parent) : QDialog(parent), context(context)
{
    _importInProcess = true;
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    QList<ImportFile> filesToImport;
    for (int i=0; i<urls->count(); i++)
        filesToImport.append(ImportFile(QFileInfo(urls->value(i).toLocalFile()).absoluteFilePath()));
    autoImportMode = false;
    autoImportStealth = false;
    init(filesToImport, context);
    _importInProcess = false;

}

RideImportWizard::RideImportWizard(QList<QString> files, Context *context, QWidget *parent) : QDialog(parent), context(context)
{
    _importInProcess = true;
    setAttribute(Qt::WA_DeleteOnClose);
    autoImportMode = false;
    autoImportStealth = false;

    QList<ImportFile> filesToImport;
    for (int i = 0; i < files.count(); i++)
        filesToImport.append(ImportFile(files.at(i)));

    init(filesToImport, context);
    _importInProcess = false;

}


RideImportWizard::RideImportWizard(RideAutoImportConfig *dirs, Context *context, QWidget *parent) : QDialog(parent), context(context), importConfig(dirs)
{
    _importInProcess = true;
    autoImportMode = true;
    autoImportStealth = true;

    if (autoImportStealth) hide();

    QList<ImportFile> files;

    // get the directories & rules
    QList<RideAutoImportRule> rules = importConfig->getConfig();

    // prepare the widget to show the status of the directory
    directoryWidget = new QTableWidget(rules.count(), TOTAL_NUM_RULE_COLUMNS, this);

    directoryWidget->verticalHeader()->setDefaultSectionSize(20 *dpiYFactor);

    QTableWidgetItem *directoryHeading = new QTableWidgetItem;
    directoryHeading->setText(tr("Directory"));
    directoryWidget->setHorizontalHeaderItem(DIRECTORY_RULE_COLUMN, directoryHeading);

    QTableWidgetItem *importRuleHeading = new QTableWidgetItem;
    importRuleHeading->setText(tr("Import Rule"));
    directoryWidget->setHorizontalHeaderItem(IMPORT_RULE_COLUMN, importRuleHeading);

    QTableWidgetItem* importFileHeading = new QTableWidgetItem;
    importFileHeading->setText(tr("Backup Files to Import"));
    directoryWidget->setHorizontalHeaderItem(COPY_ON_IMPORT_RULE_COLUMN, importFileHeading);

    QTableWidgetItem *statusHeading = new QTableWidgetItem;
    statusHeading->setText(tr("Directory Status"));
    directoryWidget->setHorizontalHeaderItem(DIRECTORY_STATUS_RULE_COLUMN, statusHeading);

    // and get the allowed files formats
    const RideFileFactory &rff = RideFileFactory::instance();
    QStringList suffixList = rff.suffixes();
    suffixList.replaceInStrings(QRegExp("^"), "*.");
    QStringList allFormats;
    foreach(QString suffix, rff.suffixes())
        allFormats << QString("*.%1").arg(suffix);

    // Fill in the directory names and importRuleStatus
    int i=-1;
    foreach (RideAutoImportRule rule, rules){
        i++; // do it here to allow "continue" - and start with "0"
        QTableWidgetItem *t;

        // Directory
        t = new QTableWidgetItem();
        t->setText(rule.getDirectory());
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        directoryWidget->setItem(i,DIRECTORY_RULE_COLUMN,t);

        // Import Rule
        QList<QString> descriptions = rule.getRuleDescriptions();
        t = new QTableWidgetItem();
        t->setText(descriptions.at(rule.getImportRule()));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        directoryWidget->setItem(i,IMPORT_RULE_COLUMN,t);

        // Import Rule
        t = new QTableWidgetItem();
        t->setText(rule.getCopyFilesOnImportText());
        t->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        directoryWidget->setItem(i,COPY_ON_IMPORT_RULE_COLUMN,t);

        // Import Status
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        directoryWidget->setItem(i,DIRECTORY_STATUS_RULE_COLUMN,t);

        // only add files if configured to do so
        if (rule.getImportRule() == RideAutoImportRule::noImport) {
            directoryWidget->item(i,DIRECTORY_STATUS_RULE_COLUMN)->setText(tr("No import"));
            continue;
        }

        // do some checks on the directory first
        QString currentImportDirectory = rule.getDirectory();
        if (currentImportDirectory == "") {
            directoryWidget->item(i,DIRECTORY_STATUS_RULE_COLUMN)->setText(tr("No directory"));
            continue;
        }
        QDir *importDir = new QDir (currentImportDirectory);
        if (!importDir->exists()) {    // directory might not be available (USB,..)
            directoryWidget->item(i,DIRECTORY_STATUS_RULE_COLUMN)->setText(tr("Directory not available"));
            continue;
        }
        if (!importDir->isReadable()) {
            directoryWidget->item(i,DIRECTORY_STATUS_RULE_COLUMN)->setText(tr("Directory not readable"));
            continue;
        }

        // determine timerange in the past which should considerd in import
        QDate selectAfter = QDate::currentDate();
        switch(rule.getImportRule()) {
        case RideAutoImportRule::importLast30days:
        case RideAutoImportRule::importBackground30:
            selectAfter = selectAfter.addDays(Q_INT64_C(-30));
            break;
        case RideAutoImportRule::importLast90days:
        case RideAutoImportRule::importBackground90:
            selectAfter = selectAfter.addDays(Q_INT64_C(-90));
            break;
        case RideAutoImportRule::importLast180days:
        case RideAutoImportRule::importBackground180:
            selectAfter = selectAfter.addDays(Q_INT64_C(-180));
            break;
        case RideAutoImportRule::importLast360days:
        case RideAutoImportRule::importBackground360:
            selectAfter = selectAfter.addDays(Q_INT64_C(-360));
            break;
        }

        // if any of the rules says "with Dialog" then we keep the dialog - if not it's stealth
        switch (rule.getImportRule()) {

        case RideAutoImportRule::importAll:
        case RideAutoImportRule::importLast30days:
        case RideAutoImportRule::importLast90days:
        case RideAutoImportRule::importLast180days:
        case RideAutoImportRule::importLast360days:
            autoImportStealth = false;
            break;
        }

        // now get the files with their full names      
        QFileInfoList fileInfos = importDir->entryInfoList(allFormats, QDir::Files, QDir::NoSort);
        if (!fileInfos.isEmpty()) {
            int j = 0;
            foreach(QFileInfo f, fileInfos) {
                // append following the import rules
                switch (rule.getImportRule()) {
                case RideAutoImportRule::importAll:
                case RideAutoImportRule::importBackgroundAll:
                    files.append(ImportFile(f.absoluteFilePath(), rule.getCopyFilesOnImport()));
                    j++;
                    break;
                case RideAutoImportRule::importLast30days:
                case RideAutoImportRule::importLast90days:
                case RideAutoImportRule::importLast180days:
                case RideAutoImportRule::importLast360days:
                case RideAutoImportRule::importBackground30:
                case RideAutoImportRule::importBackground90:
                case RideAutoImportRule::importBackground180:
                case RideAutoImportRule::importBackground360:
                    if (f.created().date() >= selectAfter || f.lastModified().date() >= selectAfter) {
                        files.append(ImportFile(f.absoluteFilePath(), rule.getCopyFilesOnImport()));
                        j++;
                    };
                    break;
                }
            }
            if (j > 0) {
                directoryWidget->item(i,3)->setText(tr("%1 files for import selected").arg(QString::number(j)));
            } else {
                directoryWidget->item(i,3)->setText(tr("No files in requested time range"));
            }
        } else {
            directoryWidget->item(i,3)->setText(tr("No activity files found"));
            continue;
        }
    }

    directoryWidget->setColumnWidth(DIRECTORY_RULE_COLUMN, 400 *dpiXFactor);
    directoryWidget->setColumnWidth(IMPORT_RULE_COLUMN, 160 *dpiXFactor);
    directoryWidget->setColumnWidth(COPY_ON_IMPORT_RULE_COLUMN, 140 *dpiXFactor);
    directoryWidget->setColumnWidth(DIRECTORY_STATUS_RULE_COLUMN, 220 * dpiXFactor);

    init(files, context);

    _importInProcess = false;


}

void
RideImportWizard::init(QList<ImportFile> original, Context * /*mainWindow*/)
{
    filenames.clear();

    // expand files if they are archives - this may involve unzipping or extracting
    //                                     files into a subdirectory, so we also clean-up
    //                                     before we close.
    QList<ImportFile> files = expandFiles(original);

    // setup Help
    HelpWhatsThis *help = new HelpWhatsThis(this);
    this->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::MenuBar_Activity_Import));

    // initialise dialog box
    tableWidget = new QTableWidget(files.count(), TOTAL_NUM_COLUMNS, this);

    tableWidget->setItemDelegate(new RideDelegate(DATE_COLUMN)); // use a delegate for column 1 date
    tableWidget->verticalHeader()->setDefaultSectionSize(20 *dpiXFactor);
    phaseLabel = new QLabel;
    progressBar = new QProgressBar();
    todayButton = new QComboBox();
    todayButton->addItem(tr("Select Date..."));
    todayButton->addItem(tr("Today"));
    todayButton->addItem(tr("Last Monday"));
    todayButton->addItem(tr("Last Tuesday"));
    todayButton->addItem(tr("Last Wednesday"));
    todayButton->addItem(tr("Last Thursday"));
    todayButton->addItem(tr("Last Friday"));
    todayButton->addItem(tr("Last Saturday"));
    todayButton->addItem(tr("Last Sunday"));
    todayButton->addItem(tr("Choose Date"));
    cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setAutoDefault(false);
    abortButton = new QPushButton(tr("Abort"));
    //overFiles = new QCheckBox(tr("Overwrite Existing Files"));  // deprecate for this release... XXX
    // initially the cancel, overwrite and today widgets are hidden
    // they only appear whilst we are asking the user for dates
    cancelButton->setHidden(true);
    todayButton->setHidden(true);
    //overFiles->setHidden(true);  // deprecate for this release... XXX
    //overwriteFiles = false;

    aborted = false;

    // NOTE: abort button morphs into save and finish button later
    connect(abortButton, SIGNAL(clicked()), this, SLOT(abortClicked()));

    // only used when editing dates
    connect(todayButton, SIGNAL(activated(int)), this, SLOT(todayClicked(int)));
    connect(cancelButton, SIGNAL(clicked()), this, SLOT(cancelClicked()));
    // connect(overFiles, SIGNAL(clicked()), this, SLOT(overClicked()));  // deprecate for this release... XXX

    // title & headings
    setWindowTitle(tr("Import Files"));
    QTableWidgetItem *filenameHeading = new QTableWidgetItem;
    filenameHeading->setText(tr("Filename"));
    tableWidget->setHorizontalHeaderItem(FILENAME_COLUMN, filenameHeading);

    QTableWidgetItem *dateHeading = new QTableWidgetItem;
    dateHeading->setText(tr("Date"));
    tableWidget->setHorizontalHeaderItem(DATE_COLUMN, dateHeading);

    QTableWidgetItem *timeHeading = new QTableWidgetItem;
    timeHeading->setText(tr("Time"));
    tableWidget->setHorizontalHeaderItem(TIME_COLUMN, timeHeading);

    QTableWidgetItem *durationHeading = new QTableWidgetItem;
    durationHeading->setText(tr("Duration"));
    tableWidget->setHorizontalHeaderItem(DURATION_COLUMN, durationHeading);

    QTableWidgetItem *distanceHeading = new QTableWidgetItem;
    distanceHeading->setText(tr("Distance"));
    tableWidget->setHorizontalHeaderItem(DISTANCE_COLUMN, distanceHeading);
    
    QTableWidgetItem* copyOnImportHeading = new QTableWidgetItem;
    copyOnImportHeading->setText(tr("Backup"));
    tableWidget->setHorizontalHeaderItem(COPY_ON_IMPORT_COLUMN, copyOnImportHeading);

    QTableWidgetItem *statusHeading = new QTableWidgetItem;
    statusHeading->setText(tr("Import Status"));
    tableWidget->setHorizontalHeaderItem(STATUS_COLUMN, statusHeading);

    // save target dir for the file import
    this->homeImports = context->athlete->home->imports();
    this->homeActivities = context->athlete->home->activities();
    this->tmpActivities = context->athlete->home->tmpActivities();

    // Fill in the filenames and all the textItems
    for (int i=0; i < files.count(); i++) {
        QTableWidgetItem *t;

        filenames.append(ImportFile(QFileInfo(files[i].name).canonicalFilePath(), files[i].copyOnImport));
        blanks.append(true); // by default editable

        // Filename
        t = new QTableWidgetItem();
        if (autoImportMode)
            t->setText(QFileInfo(files[i].name).canonicalFilePath());
        else
            t->setText(QFileInfo(files[i].name).fileName());
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        tableWidget->setItem(i,FILENAME_COLUMN,t);

        // Date
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags()  | Qt::ItemIsEditable);
        t->setBackgroundColor(Qt::red);
        tableWidget->setItem(i,DATE_COLUMN,t);

        // Time
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() | Qt::ItemIsEditable);
        tableWidget->setItem(i,TIME_COLUMN,t);

        // Duration
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        tableWidget->setItem(i,DURATION_COLUMN,t);

        // Distance
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        tableWidget->setItem(i,DISTANCE_COLUMN,t);

        // Copy on Import
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        tableWidget->setItem(i,COPY_ON_IMPORT_COLUMN, t);

        // Import Status
        t = new QTableWidgetItem();
        t->setText(tr(""));
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        tableWidget->setItem(i,STATUS_COLUMN,t);
    }

    // put into our dialog box
    // layout
    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addWidget(phaseLabel);
    buttons->addStretch();
    buttons->addWidget(todayButton);
    buttons->addStretch();
    // buttons->addWidget(overFiles); // deprecate for this release... XXX
    buttons->addWidget(cancelButton);
    buttons->addWidget(abortButton);

    QVBoxLayout *contents = new QVBoxLayout(this);
    if (autoImportMode) {
        contents->addWidget(directoryWidget);
        // only show table if files are available in autoimportMode
        if (files.count() == 0) {
            tableWidget->setVisible(false);
            progressBar->setVisible(false);
        }
    }
    contents->addWidget(tableWidget);
    contents->addWidget(progressBar);
    contents->addLayout(buttons);
    setLayout(contents);

    // adjust all the sizes to look tidy
    tableWidget->setColumnWidth(FILENAME_COLUMN, 200*dpiXFactor);
    tableWidget->setColumnWidth(DATE_COLUMN, 90*dpiXFactor);
    tableWidget->setColumnWidth(TIME_COLUMN, 90*dpiXFactor);
    tableWidget->setColumnWidth(DURATION_COLUMN, 90*dpiXFactor);
    tableWidget->setColumnWidth(DISTANCE_COLUMN, 70*dpiXFactor);
    tableWidget->setColumnWidth(COPY_ON_IMPORT_COLUMN, 70*dpiXFactor);
    tableWidget->setColumnWidth(STATUS_COLUMN, 250*dpiXFactor);
    tableWidget->horizontalHeader()->setStretchLastSection(true);

    // max height for 16 items and a scrollbar on right if > 16 items
    // for some reason the window is wider for 10-16 items too.
    // someone that understands width/geometry and layouts better
    // than me should clean up the logic here, no doubt using a
    // single call to geometry/hint. But for now it looks good
    // on Leopard, no doubt not so good on Windows
    resize((920 +
           ((files.count() > 16 ? 24 : 0) +
           ((files.count() > 9 && files.count() < 17) ? 8 : 0)))*dpiXFactor,
           (118 + ((files.count() > 16 ? 17*20 : (files.count()+1) * 20)
           + ((autoImportMode) ? 100 : 0)))*dpiYFactor); // assume not more the 5 directory in average

    if (autoImportMode) directoryWidget->adjustSize();
    tableWidget->adjustSize();

    // set number of files / so that a caller of the constructor can decide what to do
    numberOfFiles = files.count();
}

QList<ImportFile>
RideImportWizard::expandFiles(QList<ImportFile> files)
{
    // we keep a list of what we're returning
    QList<ImportFile> expanded;
    QRegExp archives("^(zip|gzip)$",  Qt::CaseInsensitive);

    foreach(ImportFile impFile, files) {

        if (archives.exactMatch(QFileInfo(impFile.name).suffix())) {
            // its an archive so lets check - but only to one depth
            // archives that contain archives can get in the sea
            QList<QString> contents = Archive::dir(impFile.name);
            if (contents.count() == 0) {
                expanded.append(impFile);
            } else {
                // we need to extract the contents and return those
                QStringList ex = Archive::extract(impFile.name, contents, const_cast<AthleteDirectoryStructure*>(context->athlete->directoryStructure())->tmpActivities().absolutePath());
                deleteMe += ex;
                foreach(QString exfile, ex) { expanded.append(ImportFile(exfile, impFile.copyOnImport)); }
            }

        } else {
            expanded.append(impFile);
        }
    }

    return expanded;
}

int
RideImportWizard::getNumberOfFiles() {
    return numberOfFiles;
}

int
RideImportWizard::process()
{

    // import process is starting
    _importInProcess = true;

    // Make visible and put in front prior to running down the list & processing...
    if (!autoImportStealth) {
        if (!isActiveWindow()) activateWindow();
        this->show();
    }

    // set progress bar limits - for each file we
    // will make 5 passes over the files
    //         1. checking if a file is readable
    //         2. parsing it with the RideFileReader
    //         3. [optional] collect date/time information from user
    //         4. copy file into Library
    //         5. Process for CPI (not implemented yet)

    // So, therefore the progress bar runs from 0 to files*4. (since step 5 is not implemented yet)
    progressBar->setMinimum(0);
    progressBar->setMaximum(filenames.count()*4);

    // Pass one - Is it valid?
    phaseLabel->setText(tr("Step 1 of 4: Check file permissions"));
    for (int i=0; i < filenames.count(); i++) {

        // get fullpath name for processing
        QFileInfo thisfile(filenames[i].name);
        if (!thisfile.exists())  tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - File does not exist."));
        else if (!thisfile.isFile())  tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - Not a file."));
        else if (!thisfile.isReadable())  tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - File is not readable."));
        else if (thisfile.fileName().endsWith("json") && thisfile.fileName().startsWith("{"))  tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - Opendata summary."));
        else {

            // is it one we understand ?
            QStringList suffixList = RideFileFactory::instance().suffixes();
            QRegExp suffixes(QString("^(%1)$").arg(suffixList.join("|")));
            suffixes.setCaseSensitivity(Qt::CaseInsensitive);

            // strip off gz or zip as openRideFile will sort that for us
            // since some file names contain "." as separator, not only for suffixes,
            // find the file-type suffix in a 2 step approach
            QStringList allNameParts = thisfile.fileName().split(".");
            QString suffix = tr("undefined");
            if (!allNameParts.isEmpty()) {
                if (allNameParts.last().toLower() == "zip" ||
                    allNameParts.last().toLower() == "gz") {
                    // gz/zip are handled by openRideFile
                    allNameParts.removeLast();
                }
                if (!allNameParts.isEmpty()) {
                    suffix = allNameParts.last();
                }
            }

            if (suffixes.exactMatch(suffix)) {

                // Woot. We know how to parse this baby
                tableWidget->item(i,STATUS_COLUMN)->setText(tr("Queued"));

            } else {
                tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - Unknown file type") + ": " + suffix);
            }
        }
        progressBar->setValue(progressBar->value()+1);

    }

    if (aborted) { done(0); return 0; }
    repaint();
    QApplication::processEvents();

    // Pass 2 - Read in with the relevant RideFileReader method

    phaseLabel->setText(tr("Step 2 of 4: Validating Files"));
   for (int i=0; i< filenames.count(); i++) {


        // does the status say Queued?
        if (!tableWidget->item(i,STATUS_COLUMN)->text().startsWith(tr("Error"))) {

              QStringList errors;
              QFile thisfile(filenames[i].name);

              tableWidget->item(i,STATUS_COLUMN)->setText(tr("Parsing..."));
              tableWidget->setCurrentCell(i,STATUS_COLUMN);
              QApplication::processEvents();

              if (aborted) { done(0); return 0; }
              this->repaint();
              QApplication::processEvents();

              QList<RideFile*> rides;
              RideFile *ride = RideFileFactory::instance().openRideFile(context, thisfile, errors, &rides);

              // is this an archive of files?
              if (rides.count() > 1) {

                 int here = i;

                 // remove current filename from state arrays and tableview
                 filenames.removeAt(here);
                 blanks.removeAt(here);
                 tableWidget->removeRow(here);

                 // resize dialog according to the number of rows we expect
                 int willhave = filenames.count() + rides.count();
                 resize((920 + ((willhave > 16 ? 24 : 0) +
                     ((willhave > 9 && willhave < 17) ? 8 : 0)))*dpiXFactor,
                     (118 + ((willhave > 16 ? 17*20 : (willhave+1) * 20)))*dpiYFactor);


                 // ok so create a temporary file and add to the tableWidget
                 // we write as JSON to ensure we don't lose data e.g. XDATA.
                 int counter = 0;
                 foreach(RideFile *extracted, rides) {

                     // write as a temporary file, using the original
                     // filename with "-n" appended
                     QString fulltarget = QDir::tempPath() + "/" + QFileInfo(thisfile).baseName() + QString("-%1.json").arg(counter+1);
                     JsonFileReader reader;
                     QFile target(fulltarget);
                     reader.writeRideFile(context, extracted, target);
                     deleteMe.append(fulltarget);
                     delete extracted;
                     
                     // now add each temporary file ...
                     filenames.insert(here, fulltarget);
                     blanks.insert(here, true); // by default editable
                     tableWidget->insertRow(here+counter);

                     QTableWidgetItem *t;

                     // Filename
                     t = new QTableWidgetItem();
                     t->setText(fulltarget);
                     t->setFlags(t->flags() & (~Qt::ItemIsEditable));
                     tableWidget->setItem(here+counter,FILENAME_COLUMN,t);

                     // Date
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags()  | Qt::ItemIsEditable);
                     t->setBackgroundColor(Qt::red);
                     tableWidget->setItem(here+counter,DATE_COLUMN,t);

                     // Time
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags() | Qt::ItemIsEditable);
                     tableWidget->setItem(here+counter,TIME_COLUMN,t);

                     // Duration
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags() & (~Qt::ItemIsEditable));
                     tableWidget->setItem(here+counter,DURATION_COLUMN,t);

                     // Distance
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags() & (~Qt::ItemIsEditable));
                     tableWidget->setItem(here+counter,DISTANCE_COLUMN,t);
                     
                     // Copy on Import
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags() & (~Qt::ItemIsEditable));
                     tableWidget->setItem(here+counter, COPY_ON_IMPORT_COLUMN, t);

                     // Import Status
                     t = new QTableWidgetItem();
                     t->setText(tr(""));
                     t->setFlags(t->flags() & (~Qt::ItemIsEditable));
                     tableWidget->setItem(here+counter,STATUS_COLUMN,t);

                     counter++;

                     tableWidget->adjustSize();
                 }
                 QApplication::processEvents();


                 // progress bar needs to adjust...
                 progressBar->setMaximum(filenames.count()*4);

                 // then go back one and re-parse from there
                 rides.clear();
   
                 i--;
                 goto next; // buttugly I know, but count em across 100,000 lines of code

              }

              // did it parse ok?
              if (ride) {

                   // ride != NULL but !errors.isEmpty() means they're just warnings
                   if (errors.isEmpty())
                       tableWidget->item(i,STATUS_COLUMN)->setText(tr("Validated"));
                   else {
                       tableWidget->item(i,STATUS_COLUMN)->setText(tr("Warning - ") + errors.join(tr(";")));
                   }

                   // Set Date and Time
                   if (!ride->startTime().isValid()) {

                       // Poo. The user needs to supply the date/time for this ride
                       blanks[i] = true;
                       tableWidget->item(i,DATE_COLUMN)->setText(tr(""));
                       tableWidget->item(i,TIME_COLUMN)->setText(tr(""));

                   } else {

                       // Cool, the date and time was extracted from the source file
                       blanks[i] = false;
                       tableWidget->item(i,DATE_COLUMN)->setText(ride->startTime().date().toString(Qt::ISODate));
                       tableWidget->item(i,TIME_COLUMN)->setText(ride->startTime().toString("hh:mm:ss"));
                   }

                   tableWidget->item(i,DATE_COLUMN)->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter); // put in the middle
                   tableWidget->item(i,TIME_COLUMN)->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter); // put in the middle

                   // time and distance from tags (.gc files)
                   QMap<QString,QString> lookup;
                   lookup = ride->metricOverrides.value("total_distance");
                   double km = lookup.value("value", "0.0").toDouble();

                   lookup = ride->metricOverrides.value("workout_time");
                   int secs = lookup.value("value", "0.0").toDouble();

                   // show duration by looking at last data point
                   if (!ride->dataPoints().isEmpty() && ride->dataPoints().last() != NULL) {
                       if (!secs) secs = ride->dataPoints().last()->secs + ride->recIntSecs();
                       if (!km) km = ride->dataPoints().last()->km;
                   }

                   QChar zero = QLatin1Char ( '0' );
                   QString time = QString("%1:%2:%3").arg(secs/3600,2,10,zero)
                       .arg(secs%3600/60,2,10,zero)
                       .arg(secs%60,2,10,zero);
                   tableWidget->item(i,DURATION_COLUMN)->setText(time);
                   tableWidget->item(i,DURATION_COLUMN)->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter); // put in the middle

                   // show distance by looking at last data point
                   QString dist = GlobalContext::context()->useMetricUnits
                       ? QString ("%1 km").arg(km, 0, 'f', 1)
                       : QString ("%1 mi").arg(km * MILES_PER_KM, 0, 'f', 1);
                   tableWidget->item(i,DISTANCE_COLUMN)->setText(dist);
                   tableWidget->item(i,DISTANCE_COLUMN)->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

                   // Add copy on import information
                   tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(filenames[i].copyOnImport ? "Yes" : "No");
                   tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter); // put in the middle

                   delete ride;
               } else {
                   // nope - can't handle this file
                   tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - ") + errors.join(tr(";")));
               }
        }
        progressBar->setValue(progressBar->value()+1);
        QApplication::processEvents();
        if (aborted) { done(0); return 0; }
        this->repaint();

        next:;
    }

    // Pass 3 - get missing date and times for imported files
    //         Actually allow us to edit date on ANY ride, we
    //         make sure that the ride date/time is set from
    //         the filename and never from the ride data

    int needdates=0;
    for (int i=0; i<filenames.count(); i++) {

        // ignore errors
        QTableWidgetItem *t = tableWidget->item(i,STATUS_COLUMN);
        if (t->text().startsWith(tr("Error"))) continue;

        if (blanks[i]) needdates++; // count the blanks tho -- these MUST be edited

        // does nothing for the moment
        progressBar->setValue(progressBar->value()+1);
        progressBar->repaint();
   }

   // lets make the text more helpful!
   if (needdates) {
     phaseLabel->setText(QString(tr("Step 3 of 4: %1 ride(s) are missing the date and time.")).arg(needdates));
   } else {
     phaseLabel->setText(tr("Step 3 of 4: Change Date/Time or Save to continue."));
   }

   // get it on top to save / correct missing dates
   if (autoImportStealth && needdates > 0) {
       // leave the stealth mode
       this->show();
       activateWindow();
   } else {
       if (!isActiveWindow()) activateWindow();
   }
   // Wait for user to press save
   abortButton->setText(tr("Save"));
   aborted = false;

   cancelButton->setHidden(false);
   todayButton->setHidden(false);
   //overFiles->setHidden(false); // deprecate for this release... XXX

   if (needdates == 0) {
      // no need to wait for the user to input dates
      // and press save if all the dates are set
      // (i.e. they got set by the file importers already)
      // do nothing for now since we need to confirm dates
      // and confirm overwrite files rather than importing
      // without user intervention

      abortButton->setDisabled(false);
      activateSave();
      if (autoImportStealth) abortClicked();  // simulate "Save" by User

   } else {

      // de-activate Save button until the dates and times are sorted
      abortButton->setDisabled(true);
   }
   connect(tableWidget, SIGNAL(itemChanged(QTableWidgetItem *)), this, SLOT(activateSave()));

   // in autoimport mode / no files, skip "Save",... and goto "Finish"
   if (autoImportMode && filenames.count()== 0) {
       cancelButton->setHidden(true);
       todayButton->setHidden(true);
       abortButton->setDisabled(false);
       phaseLabel->setText(tr("No files for automatic import selected."));
       abortButton->setText(tr("Finish"));
       aborted = false;
   }

   return 0;
}

//void
//RideImportWizard::overClicked()
//{
    //overwriteFiles = overFiles->isChecked(); //deprecate in this release XXX
//}

void
RideImportWizard::activateSave()
{

   for (int i=0; i<filenames.count(); i++) {

        // ignore errors
        QTableWidgetItem *t = tableWidget->item(i,STATUS_COLUMN);
        if (t->text().startsWith(tr("Error"))) continue;

       // date needed?
        t = tableWidget->item(i,DATE_COLUMN);
        if (t->text() == "") return;

        // time needed?
        t = tableWidget->item(i,TIME_COLUMN);
        if (t->text() == "") return;
   }
   // if you got here then all entries that need a date have a date
   abortButton->setDisabled(false);
}

// the code here is tedious. checking for conditions about total ride time
// and if it fits in a day, then distributing them over the day, but
// only if the time is not set, and then if its being set to today
// then start must be before the current time cause you can't log
// data for future rides ... and so on.
//
// but then this feature is gonna save a lot of typing over time ...
//
void
RideImportWizard::todayClicked(int index)
{
    QDate selectedDate; // the date we're gonna apply to the row(s) highlighted

    // set the index back to 0, so we can select again
    todayButton->setCurrentIndex(0);

    // 0 = nothing selected, 1 = Today - 2 = last monday thru to 8 = last sunday
    if (index == 0) { // no selection
        return;
    } else if (index == 1) { // today
        selectedDate = QDate().currentDate();
    } else if (index == 9) { // other date - set focus on first highlighted date
        for (int i=0; i<filenames.count(); i++) {
            if (tableWidget->item(i,FILENAME_COLUMN)->isSelected() ||
                tableWidget->item(i,DATE_COLUMN)->isSelected() ||
                tableWidget->item(i,TIME_COLUMN)->isSelected() ||
                tableWidget->item(i,DURATION_COLUMN)->isSelected() ||
                tableWidget->item(i,DISTANCE_COLUMN)->isSelected() ||
                tableWidget->item(i,COPY_ON_IMPORT_COLUMN)->isSelected() ||
                tableWidget->item(i,STATUS_COLUMN)->isSelected()) {
                tableWidget->editItem(tableWidget->item(i,DATE_COLUMN));
                return;
            }
        }
        return;
    } else { // last mon/tue/wed/thu/fri/sat/sun
        int daysago =  (QDate().currentDate().dayOfWeek() - index + 8) % 7;
        if (!daysago) daysago=7; // e.g.last wednesday is 7 days ago if today is wednesday
        selectedDate = QDate().fromJulianDay(QDate().currentDate().toJulianDay()-daysago);
    }

    // Only apply to selected date - set time to current time - ride duration
    // pretty daft but at least it sets it to something, anything is gonna be random
    int countselected = 0;
    int totalduration = 0;
    for (int i=0; i< filenames.count(); i++) {
        if (tableWidget->item(i,FILENAME_COLUMN)->isSelected() ||
            tableWidget->item(i,DATE_COLUMN)->isSelected() ||
            tableWidget->item(i,TIME_COLUMN)->isSelected() ||
            tableWidget->item(i,DURATION_COLUMN)->isSelected() ||
            tableWidget->item(i,DISTANCE_COLUMN)->isSelected() ||
            tableWidget->item(i,COPY_ON_IMPORT_COLUMN)->isSelected() ||
            tableWidget->item(i,STATUS_COLUMN)->isSelected()) {
            countselected++;

            QTime duration = QTime().fromString(tableWidget->item(i,DURATION_COLUMN)->text(), "hh:mm:ss");
            totalduration += duration.hour() * 3600 +
                             duration.minute() * 60 +
                             duration.second();
        }
    }

    // More than a days worth of rides so can't squeeze into a single day!
    if (totalduration > (24 * 3600)) {
        QMessageBox::warning ( this, tr ( "Invalid Selection" ), tr ( "More than 24hrs of activities to fit into a day" ));
        return;
    }

    // if it is today then start from now - total rides duration
    // if that goes negative then just do the std thing
    int rstart=0;
    if (index == 1) {

        QTime timenow = QTime::currentTime();

        int now  = timenow.hour() * 3600 +
                   timenow.minute() * 60 +
                   timenow.second();

        rstart = now - totalduration;

    }

    if (rstart <= 0) { // i.e. still not set properly...
        // if it is not being set to "today" then spread them across the
        // day back to back with equal time before and after noon.
        rstart = ((24*3600) - totalduration) /2;
    }

    // zip through the rows and where highlighted set the date
    // if the start time is not set set it to rstart and increment
    // by the duration of the ride.
    for (int i=0; i< filenames.count(); i++) {
        if (tableWidget->item(i,FILENAME_COLUMN)->isSelected() ||
            tableWidget->item(i,DATE_COLUMN)->isSelected() ||
            tableWidget->item(i,TIME_COLUMN)->isSelected() ||
            tableWidget->item(i,DURATION_COLUMN)->isSelected() ||
            tableWidget->item(i,DISTANCE_COLUMN)->isSelected() ||
            tableWidget->item(i,COPY_ON_IMPORT_COLUMN)->isSelected() ||
            tableWidget->item(i,STATUS_COLUMN)->isSelected()) {

            // set the date to date selected
            tableWidget->item(i,DATE_COLUMN)->setText(selectedDate.toString(Qt::ISODate));
            // look at rides with missing start time - we need to populate those

            // ride duration
            QTime duration = QTime().fromString(tableWidget->item(i,DURATION_COLUMN)->text(), "hh:mm:ss");

            // ride start time
            QTime time(rstart/3600, rstart%3600/60, rstart%60);
            tableWidget->item(i,TIME_COLUMN)->setText(time.toString("hh:mm:ss"));
            rstart += duration.hour() * 3600 +
                      duration.minute() * 60 +
                      duration.second();
        }
    }
    // phew! - repaint!
    QApplication::processEvents();
    tableWidget->repaint();
}

void
RideImportWizard::cancelClicked()
{
    done(0); // you are the weakest link, goodbye.
}

// info structure used by cpi updater
struct cpi_file_info {
    QString file, inname, outname;
};


void
RideImportWizard::abortClicked()
{
    // NOTE: abort button morphs into save and finish button later - so all 3 variants are processed here

    // if done when labelled abort we kill off this dialog
    // & removed to avoid issues with kde AutoCheckAccelerators
    QString label = QString(abortButton->text()).replace("&", "");

    // Process "ABORT"
    if (label == tr("Abort")) {
        hide();
        aborted=true; // terminated. I'll be back.
        return;
    }

    // Process "FINISH"
    if (label == tr("Finish")) {
       // phew. our work is done. -- lets force an update stats...
       hide();
       if (autoImportStealth) {
           // inform the user that the work is done
           QMessageBox::information(NULL, tr("Auto Import"), tr("Automatic import from defined directories is completed."));
       }
       done(0);
       return;
    }

    // Process "SAVE"

    // SAVE STEP 2 - set the labels and make the text un-editable
    phaseLabel->setText(tr("Step 4 of 4: Save to Library"));

    abortButton->setText(tr("Abort"));
    aborted = false;
    cancelButton->setHidden(true);
    todayButton->setHidden(true);
    //overFiles->setHidden(true);  // deprecate for this release XXX

    // now set this fields uneditable again ... yeesh.
    for (int i=0; i <filenames.count(); i++) {
        QTableWidgetItem *t = tableWidget->item(i,DATE_COLUMN);
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
        t = tableWidget->item(i,TIME_COLUMN);
        t->setFlags(t->flags() & (~Qt::ItemIsEditable));
    }

    QChar zero = QLatin1Char ( '0' );

    // Count the number of successfully imported files
    int successfulImports = 0;

    // Saving now - process the files one-by-one
    for (int i=0; i< filenames.count(); i++) {

        if (tableWidget->item(i, STATUS_COLUMN)->text().startsWith(tr("Error"))) {
            tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("No"));
            continue; } // skip errors

        tableWidget->item(i,STATUS_COLUMN)->setText(tr("Saving..."));
        tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("---"));
        tableWidget->setCurrentCell(i, STATUS_COLUMN);
        QApplication::processEvents();
        if (aborted) { done(0); return; }
        this->repaint();


        // SAVE STEP 3 - prepare the new file names for the next steps - basic name and .JSON in GC format

        QDateTime ridedatetime = QDateTime(QDate().fromString(tableWidget->item(i,DATE_COLUMN)->text(), Qt::ISODate),
                                           QTime().fromString(tableWidget->item(i,TIME_COLUMN)->text(), "hh:mm:ss"));
        QString targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                .arg ( ridedatetime.date().year(), 4, 10, zero )
                .arg ( ridedatetime.date().month(), 2, 10, zero )
                .arg ( ridedatetime.date().day(), 2, 10, zero )
                .arg ( ridedatetime.time().hour(), 2, 10, zero )
                .arg ( ridedatetime.time().minute(), 2, 10, zero )
                .arg ( ridedatetime.time().second(), 2, 10, zero );
        QString activitiesTarget = QString ("%1.%2" ).arg ( targetnosuffix ).arg ( "json" );

        // create filenames incl. directory path for GC .JSON for both /tmpActivities and /activities directory
        QString tmpActivitiesFulltarget = tmpActivities.canonicalPath() + "/" + activitiesTarget;
        QString finalActivitiesFulltarget = homeActivities.canonicalPath() + "/" + activitiesTarget;

        // check if a ride at this point of time already exists in /activities - if yes, skip import
        if (QFileInfo(finalActivitiesFulltarget).exists()) {
            tableWidget->item(i,STATUS_COLUMN)->setText(tr("Failed - Activity already exists in activities folder"));
            tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Not Copied"));
            continue; }

        // in addition, also check the RideCache for a Ride with the same point in Time in UTC, which also indicates
        // that there was already a ride imported - reason is that RideCache start time is in UTC, while the file Name is in "localTime"
        // which causes problems when importing the same file (for files which do not have time/date in the file name),
        // while the computer has been set to a different time zone
        if (context->athlete->rideCache->getRide(ridedatetime.toUTC())) {
            tableWidget->item(i, STATUS_COLUMN)->setText(tr("Failed - Activity with same start date/time exists in activities folder"));
            tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Not Copied")); 
            continue; }

        bool srcInImportsDir(true);
        QString importsTarget;
        QFileInfo sourceFileInfo(filenames[i].name);

        // Copy the source file to "/imports" directory (if it's not taken from there as source)
        if (sourceFileInfo.canonicalPath() != homeImports.canonicalPath()) {
            // Add the GC file base name (targetnosuffix) to create unique file names during import (for identification)
            // Note: There should not be 2 ride files with exactly the same time stamp (as this is also not foreseen for the .json)
            importsTarget = sourceFileInfo.baseName() + "_" + targetnosuffix + "." + sourceFileInfo.suffix();
            srcInImportsDir = false;
        } else
        {
            // file is re-imported from /imports - keep the name for .JSON Source File Tag
            importsTarget = sourceFileInfo.fileName();
        }

        // SAVE STEP 4 - open the file with the respective format reader and export as .JSON
        // to track if addRideCache() has caused an error due to bad data we work with a interim directory for the activities
        // -- first   export to /tmpactivities
        // -- second  create RideCache() entry
        // -- third   move file from /tmpactivities to /activities
        // -- fourth  copy source file to /imports (if required)

        // serialize the file to .JSON
        QStringList errors;
        QFile thisfile(filenames[i].name);
        RideFile *ride(RideFileFactory::instance().openRideFile(context, thisfile, errors));

        // did the input file parse ok ? (should be fine here - since it was already checked before - but just in case)
        if (ride) {

            // update ridedatetime and set the Source File name
            ride->setStartTime(ridedatetime);
            ride->setTag("Source Filename", importsTarget);
            ride->setTag("Filename", activitiesTarget);
            if (errors.count() > 0)
                ride->setTag("Import errors", errors.join("\n"));

            // process linked defaults
            GlobalContext::context()->rideMetadata->setLinkedDefaults(ride);

            // run the processor first... import
            tableWidget->item(i,STATUS_COLUMN)->setText(tr("Processing..."));
            DataProcessorFactory::instance().autoProcess(ride, "Auto", "Import");
            ride->recalculateDerivedSeries();

            tableWidget->item(i,STATUS_COLUMN)->setText(tr("Saving file..."));

            // serialize
            JsonFileReader reader;
            QFile target(tmpActivitiesFulltarget);
            if (reader.writeRideFile(context, ride, target)) {

                // now try adding the Ride to the RideCache - since this may fail due to various reason, the activity file
                // is stored in tmpActivities during this process to understand which file has create the problem when restarting GC
                // - only after the step was successful the file is moved
                // to the "clean" activities folder
                context->athlete->addRide(QFileInfo(tmpActivitiesFulltarget).fileName(),
                                          tableWidget->rowCount() < 20 ? true : false, // don't signal if mass importing
                                          true, true);                                 // file is available only in /tmpActivities, so use this one please

                // rideCache is successfully updated, let's move the file to the real /activities
                if (moveFile(tmpActivitiesFulltarget, finalActivitiesFulltarget)) {
                    tableWidget->item(i, STATUS_COLUMN)->setText(tr("Import successful - Saved in activities folder"));
                    // and correct the path locally stored in Ride Item
                    context->ride->setFileName(homeActivities.canonicalPath(), activitiesTarget);
                    // Record the successful import
                    successfulImports++;
                    // copy the source file to "/imports" directory (if it's not taken from there as source) and copying to /Imports is required
                    copySourceFileToImportDir(filenames[i], importsTarget, i, srcInImportsDir);
                }   else {
                    tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - Moving %1 to activities folder").arg(activitiesTarget));
                    tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("No"));

                }
            }   else {
                tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - .JSON creation failed"));
                tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Not Copied"));

            }
        } else {
            tableWidget->item(i,STATUS_COLUMN)->setText(tr("Error - Import of activity file failed"));
            tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Not Copied"));

        }

        // now metrics have been calculated
        DataProcessorFactory::instance().autoProcess(ride, "Save", "ADD");

        // clear
        delete ride;

        QApplication::processEvents();
        if (aborted) { done(0); return; }
        progressBar->setValue(progressBar->value()+1);
        this->repaint();
    }

    tableWidget->setSortingEnabled(true); // so you can browse through errors etc
    QString donemessage = QString(tr("Import Complete. %1 of %2 successful."))
            .arg(successfulImports, 1, 10, zero)
            .arg(filenames.count(), 1, 10, zero);
    progressBar->setValue(progressBar->maximum());
    phaseLabel->setText(donemessage);
    abortButton->setText(tr("Finish"));
    aborted = false;
    _importInProcess = false;  // Re-enable the window's close icon

    if (autoImportStealth) {
        abortClicked();  // simulate pressing the "Finish" button - even if the window got visible
    } else {
        if (!isActiveWindow()) activateWindow();
    }
}

void
RideImportWizard::copySourceFileToImportDir(const ImportFile& source, const QString& importsTarget, const int i, bool srcInImportsDir)
{
    // Copy the source file to "/imports" directory (if it's not taken from there as source) and copying to /Imports is required           
    if (!srcInImportsDir) {

        if (source.copyOnImport) {
                        
            QString importsFulltarget = homeImports.canonicalPath() + "/" + importsTarget;

            // If a file already exists in the import directory with the same name as import target, then to maintain
            // the integrity between the activities and import directories, move the exisiting import directory file
            // to a safe unique name and then store the file actually used for the import.
            if (QFile::exists(importsFulltarget)) {

                QString suf(QFileInfo(importsTarget).suffix());
                suf.prepend(".");

                QString importsTargetNoSuffix(QString(importsTarget).remove(suf, Qt::CaseSensitive));

                QString importsFulltargetDest(homeImports.canonicalPath() + "/" + importsTargetNoSuffix +
                    "_renamed_on_" + (QDateTime::currentDateTime().toString(QString("yyyy-MM-dd-hh-mm-ss"))) + suf);

                if (!moveFile(importsFulltarget, importsFulltargetDest)) {
                    // Canot move the file, although it a has same filename as import file, its contents
                    // may differ, so warn the user, this should be an extremely rare error case.
                    tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Blocked"));
                    return;
                }
            }

            // copy the source file to /imports with adjusted name
            QFile sourceFile(source.name);

            // Copy imported source file to the /imports directory
            if (sourceFile.copy(importsFulltarget)) {
                tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Copied"));
            } else {
                tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Failed"));
            }        
        } else {
            // the file is not required to be copied to imports
            tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Not Copied"));
        }
    } else {
        // the file being imported is the /import directory file ! therefore no action is required.
        tableWidget->item(i, COPY_ON_IMPORT_COLUMN)->setText(tr("Import=>"));
    }
}

bool
RideImportWizard::moveFile(const QString &source, const QString &target) {

    QFile r(source);

    // first try it with a rename
    if (r.rename(target)) return true; // job is done

    // now the harder variant (copy & delete)
    if (r.copy(target))
      {
        // try to remove - but if this fails, no problem, file has been copied at least
        r.remove();
        // even if remove failed, the copy was successful - so GC is fine
        return true;
      }

    // more required ?

    return false;

}


void
RideImportWizard::closeEvent(QCloseEvent* event)
{
    if (_importInProcess)
        event->ignore();
    else
        event->accept();
}

void
RideImportWizard::done(int rc)
{
    _importInProcess = false;
    QDialog::done(rc);
}

// clean up files
RideImportWizard::~RideImportWizard()
{
    foreach(QString name, deleteMe) QFile(name).remove();
}


// Below is the code to implement a custom ItemDelegate to support
// editing of the date and time of the Ride inside a QTableWidget
// the ItemDelegate is registered for every cell in the table
// and only kicks into life for the columns in which the date and
// time are stored, and only when the DateTime for that row is 0.

// construct the delegate - save away the column containing the date
//                          bearing in mind the time is in the next
//                          column.
RideDelegate::RideDelegate(int dateColumn, QObject *parent) : QItemDelegate(parent)
{
    this->dateColumn = dateColumn;
}


// paint the cells for date and time - pass through for all the
// other cells. Uses the model data to display.
void RideDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    if (index.column() == dateColumn) {

        QString value = index.model()->data(index, Qt::DisplayRole).toString();
        // display with angles to show its doing something
        QString text = QString("%1").arg(value);

        QStyleOptionViewItem myOption = option;
        myOption.displayAlignment = Qt::AlignHCenter | Qt::AlignVCenter;
        drawDisplay(painter, myOption, myOption.rect, text);
        drawFocus(painter, myOption, myOption.rect);

    } else if (index.column() == dateColumn+1) {

        QString value = index.model()->data(index, Qt::DisplayRole).toString();
        // display with angles to show its doing something
        QString text = QString("%1").arg(value);

        QStyleOptionViewItem myOption = option;
        myOption.displayAlignment = Qt::AlignHCenter | Qt::AlignVCenter;
        drawDisplay(painter, myOption, myOption.rect, text);
        drawFocus(painter, myOption, myOption.rect);

    } else {

        QItemDelegate::paint(painter, option, index);

    }
}

// setup editor for edit of field!!
QWidget *RideDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (index.column() == dateColumn) {

        // edit that date!
        QDateEdit *dateEdit = new QDateEdit(parent);
        dateEdit->setDisplayFormat("yyyy-MM-dd"); // ISO Format, no translation analog Qt::ISODate
        connect(dateEdit, SIGNAL(editingFinished()), this, SLOT(commitAndCloseDateEditor()));
        return dateEdit;
    } else if (index.column() == dateColumn+1) {

        // edit that time
        QTimeEdit *timeEdit = new QTimeEdit(parent);
        timeEdit->setDisplayFormat("hh:mm:ss");
        connect(timeEdit, SIGNAL(editingFinished()), this, SLOT(commitAndCloseTimeEditor()));
        return timeEdit;
    } else {
        return QItemDelegate::createEditor(parent, option, index);
    }
}

// user hit tab or return so save away the data to our model
void RideDelegate::commitAndCloseDateEditor()
{
    QDateEdit *editor = qobject_cast<QDateEdit *>(sender());
    emit commitData(editor);
    emit closeEditor(editor);
}

// user hit tab or return so save away the data to our model
void RideDelegate::commitAndCloseTimeEditor()
{
    QTimeEdit *editor = qobject_cast<QTimeEdit *>(sender());
    emit commitData(editor);
    emit closeEditor(editor);
}

// We don't set anything because the data is saved within the view not the model!
void RideDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
   // stored as text field
    if (index.column() == dateColumn) {
        QDateEdit *dateEdit = qobject_cast<QDateEdit *>(editor);
        QDate date = QDate().fromString(index.model()->data(index, Qt::DisplayRole).toString(), Qt::ISODate);
        dateEdit->setDate(date);
    } else if (index.column() == dateColumn+1) {
        QTimeEdit *timeEdit = qobject_cast<QTimeEdit *>(editor);
        QTime time = QTime().fromString(index.model()->data(index, Qt::DisplayRole).toString(), "hh:mm:ss");;
        timeEdit->setTime(time);
    }
}

// We don't set anything because the data is saved within the view not the model!
void RideDelegate::setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{

    // stored as text field
    if (index.column() == dateColumn) {
        QDateEdit *dateEdit = qobject_cast<QDateEdit *>(editor);
        QString value = dateEdit->date().toString(Qt::ISODate);
        // Place in the view
        model->setData(index, value, Qt::DisplayRole);
    } else if (index.column() == dateColumn+1) {
        QTimeEdit *timeEdit = qobject_cast<QTimeEdit *>(editor);
        QString value = timeEdit->time().toString("hh:mm:ss");
        model->setData(index, value, Qt::DisplayRole);
    }
}

