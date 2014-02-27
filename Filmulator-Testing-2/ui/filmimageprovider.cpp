#include "filmimageprovider.h"
#include <pwd.h>
#include <unistd.h>
#include <QTimer>
#include <cmath>
#define TIMEOUT 0.1

FilmImageProvider::FilmImageProvider(QQuickImageProvider::ImageType type) :
    QObject(0),
    QQuickImageProvider(type, QQuickImageProvider::ForceAsynchronousImageLoading)
{
    valid = none;

    lutR.setUnity();
    lutG.setUnity();
    lutB.setUnity();

    highlights = 0;

    exposureComp = 0.0;

    develTime = 100.0;
    agitateCount = 1;
    develSteps = 12;
    filmArea = 864.0;
    layerMixConst = 0.2;

    blackpoint = 0;
    whitepoint = .002;
    defaultToneCurveEnabled = true;

    shadowsX = 0.25;
    shadowsY = 0.25;
    highlightsX = 0.75;
    highlightsY = 0.75;

    filmLikeLUT.fill(this);
}

FilmImageProvider::FilmImageProvider() :
    QObject(0),
    QQuickImageProvider(QQuickImageProvider::Image,
                        QQuickImageProvider::ForceAsynchronousImageLoading)
{
    valid = none;

    lutR.setUnity();
    lutG.setUnity();
    lutB.setUnity();

    highlights = 0;

    exposureComp = 0.0;

    develTime = 100.0;
    agitateCount = 1;
    develSteps = 12;
    filmArea = 864.0;
    layerMixConst = 0.2;

    blackpoint = 0;
    whitepoint = .002;
    defaultToneCurveEnabled = true;

    shadowsX = 0.25;
    shadowsY = 0.25;
    highlightsX = 0.75;
    highlightsY = 0.75;

    filmLikeLUT.fill(this);
}

FilmImageProvider::~FilmImageProvider()
{
}

QImage FilmImageProvider::requestImage(const QString &id,
                                       QSize *size, const QSize &requestedSize)
{
    gettimeofday(&request_start_time,NULL);
    QImage output = emptyImage();
    QString tempID = id;
    tempID.remove(tempID.length()-1,1);
    tempID.remove(0,7);


    switch (valid)
    {
    case none://Load image into buffer
    {
        mutex.lock();
        valid = load;
        mutex.unlock();

    }
    case load://Do demosaic
    {
        //If we're about to do work on a new image, we give up.
        if(checkAbort(load))
            return emptyImage();

        //Mark that demosaicing has started.
        mutex.lock();
        valid = demosaic;
        mutex.unlock();

        //Here we trim the 'file://' from the url
        std::vector<std::string> input_filename_list;
        input_filename_list.push_back(tempID.toStdString());
        cout << "Opening " << input_filename_list[0] << endl;

        //We set the exposure comp from the Q_PROPERTY here.
        std::vector<float> input_exposure_compensation;
        input_exposure_compensation.push_back(exposureComp);


        //Only raws.
        bool tiff_in = false;
        bool jpeg_in = false;


        //Reads in the photo.
        if(imload(input_filename_list, input_exposure_compensation,
                  input_image, tiff_in, jpeg_in, exifData, highlights,
                  wbRMultiplier,wbGMultiplier,wbBMultiplier))
        {
            qDebug("Error loading images");
            mutex.lock();
            valid = none;
            mutex.unlock();
            return emptyImage();
        }
    }
    case demosaic://Do pre-filmulation work.
    {
        if(checkAbort(demosaic))
            return emptyImage();

        //Mark that we've started prefilmulation stuff.
        mutex.lock();
        valid = prefilmulation;
        mutex.unlock();

        //Here we apply the exposure compensation and white balance.
//        pre_film_image = input_image * pow(2, exposureComp);

        //This sets up the multipliers; we normalize to green.
        const float mult = pow(2,exposureComp);
        const float rMult = wbRMultiplier*pow(2,exposureComp)/wbGMultiplier;
        cout << wbRMultiplier << endl;
        cout << "rmult: " << rMult << endl;
        const float gMult = wbGMultiplier*pow(2,exposureComp)/wbGMultiplier;
        cout << wbGMultiplier << endl;
        cout << "gmult: " << gMult << endl;
        const float bMult = wbBMultiplier*pow(2,exposureComp)/wbGMultiplier;
        cout << wbBMultiplier << endl;
        cout << "bmult: " << bMult << endl;
        pre_film_image = input_image;
#pragma omp parallel for
        for(int row = 0; row < input_image.nr(); row++)
        {
            for (int col = 0; col < input_image.nc(); col += 3)
            {
                pre_film_image(row,col+0) *= mult;//rMult;
                pre_film_image(row,col+1) *= mult;//gMult;
                pre_film_image(row,col+2) *= mult;//bMult;
            }
        }

        updateFloatHistogram(preFilmHist, pre_film_image, 65535, histPreFilm);
//        cout << mean(pre_film_image) << endl;
        emit histPreFilmChanged();

    }
    case prefilmulation://Do filmulation
    {
        //If we're about to do work on invalidated demosaicing, we give up.
        //It'll restart automatically.
        if(checkAbort(prefilmulation))
            return emptyImage();

        //Mark that we've started to filmulate.
        mutex.lock();
        valid = filmulation;
        mutex.unlock();

/*        //Read in from the configuration file
        //Get home directory
        int myuid = getuid();
        passwd *mypasswd = getpwuid(myuid);
        std::string input_configuration = std::string(mypasswd->pw_dir) +
                "/.filmulator/configuration.txt";
        initialize(input_configuration, filmParams);*/

        //Set up filmulation parameters.
        filmParams.initial_developer_concentration = 1.0;
        filmParams.reservoir_thickness = 1000.0;
        filmParams.active_layer_thickness = 0.1;
        filmParams.crystals_per_pixel = 500.0;
        filmParams.initial_crystal_radius = 0.00001;
        filmParams.initial_silver_salt_density = 1.0;
        filmParams.developer_consumption_const = 2000000.0;
        filmParams.crystal_growth_const = 0.00001;
        filmParams.silver_salt_consumption_const = 2000000.0;
        filmParams.total_development_time = develTime;
        filmParams.agitate_count = agitateCount;
        filmParams.development_steps = develSteps;
        filmParams.film_area = filmArea;
        filmParams.sigma_const = 0.2;
        filmParams.layer_mix_const = layerMixConst;
        filmParams.layer_time_divisor = 20;
        filmParams.rolloff_boundary = 51275;


        //Here we do the film simulation on the image...
        if(filmulate(pre_film_image, filmulated_image, filmParams, this))
        {
            return emptyImage();//filmulate returns 1 if it detected an abort
        }

        //Histogram work
        updateFloatHistogram(postFilmHist, filmulated_image, .0025, histPostFilm);
        //cout << mean(filmulated_image) << endl;
        emit histPostFilmChanged();//must be run to notify QML
    }
    case filmulation://Do whitepoint_blackpoint
    {
        setProgress(0.8);

        //See if the filmulation has been invalidated.
        if(checkAbort(filmulation))
            return emptyImage();

        //Mark that we've begun clipping the image and converting to unsigned short.
        mutex.lock();
        valid = whiteblack;
        mutex.unlock();
        whitepoint_blackpoint(filmulated_image, contrast_image, whitepoint,
                              blackpoint);
    }
    case whiteblack: // Do color_curve
    {
        setProgress(0.85);

        //See if the clipping has been invalidated.
        if(checkAbort(whiteblack))
            return emptyImage();

        //Mark that we've begun running the individual color curves.
        mutex.lock();
        valid = colorcurve;
        mutex.unlock();
        color_curves(contrast_image, color_curve_image, lutR, lutG, lutB);
    }
    case colorcurve://Do flim-like curve
    {
        setProgress(0.9);

        //See if the color curves applied are now invalid.
        if(checkAbort(colorcurve))
            return emptyImage();

        //Mark that we've begun applying the all-color tone curve.
        mutex.lock();
        valid = filmlikecurve;
        mutex.unlock();
        filmLikeLUT.fill(this);
        film_like_curve(color_curve_image,film_curve_image,filmLikeLUT);
    }
    case filmlikecurve: //output
    {
        setProgress(0.95);

        //See if the tonecurve has changed since it was applied.
        if(checkAbort(filmlikecurve))
            return emptyImage();

        //We would mark our progress, but this is the very last step.

        int nrows = film_curve_image.nr();
        int ncols = film_curve_image.nc();
        switch((int) exifData["Exif.Image.Orientation"].value().toLong())
        {
        case 3://upside-down
        {
            //Yes, some cameras do in fact implement this direction.
            output = QImage(ncols/3,nrows,QImage::Format_ARGB32);
            for(int i = 0; i < nrows; i++)
            {
                // out   in
                /* 12345 00000
                 * 00000 00000
                 * 00000 54321
                 */
                //Reversing the row index
                int r = nrows-1-i;
                QRgb *line = (QRgb *)output.scanLine(i);
                for(int j = 0; j < ncols; j = j + 3)
                {
                    //Reversing the column index
                    int c = ncols - 3 - j;
                    *line = QColor(film_curve_image(r,c)/256,
                                   film_curve_image(r,c+1)/256,
                                   film_curve_image(r,c+2)/256).rgb();
                    line++;
                }
            }
            break;
        }
        case 6://right side of camera down
        {
            output = QImage(nrows,ncols/3,QImage::Format_ARGB32);
            for(int j = 0; j < ncols/3; j++)
            {
                //index of an output row as a column on the input matrix
                // out   in
                /* 123   30000
                 * 000   20000
                 * 000   10000
                 * 000
                 * 000
                 */
                //Remember that the columns are interlaced.
                int c = j*3;
                QRgb *line = (QRgb *)output.scanLine(j);
                for(int i = 0; i < nrows; i++)
                {
                    //Also, in this case, the order is reversed for the rows of the input.
                    int r = nrows-1-i;
                    *line = QColor(film_curve_image(r,c)/256,
                                   film_curve_image(r,c+1)/256,
                                   film_curve_image(r,c+2)/256).rgb();
                    line++;
                }
            }
            break;
        }
        case 8://right side of camera up
        {
            output = QImage(nrows,ncols/3,QImage::Format_ARGB32);
            for(int j = 0; j < ncols/3; j++)
            {
                //index of an output row as a column on the input matrix
                // out   in
                /* 123   00001
                 * 000   00002
                 * 000   00003
                 * 000
                 * 000
                 */
                //Remember that the columns are interlaced, and scanned in reverse.
                int c = ncols - 3 - j*3;
                QRgb *line = (QRgb *)output.scanLine(j);
                for(int i = 0; i < nrows; i++)
                {
                    //Here the order is not reversed for the rows of the input.
                    int r = i;
                    *line = QColor(film_curve_image(r,c)/256,
                                   film_curve_image(r,c+1)/256,
                                   film_curve_image(r,c+2)/256).rgb();
                    line++;
                }
            }
            break;
        }
        default://standard orientation
        {
            output = QImage(ncols/3,nrows,QImage::Format_ARGB32);
            for(int i = 0; i < nrows; i++)
            {
                QRgb *line = (QRgb *)output.scanLine(i);
                for(int j = 0; j < ncols; j = j + 3)
                {
                    *line = QColor(film_curve_image(i,j)/256,
                                   film_curve_image(i,j+1)/256,
                                   film_curve_image(i,j+2)/256).rgb();
                    line++;

                }
            }
        }
        }//End orientation switch
    }
    }//End task switch

    emit histPostFilmChanged();
    updateShortHistogram(finalHist, film_curve_image, histFinal);
    emit histFinalChanged();//This must be run immediately after updateHistFinal in order to notify QML.

    tout << "Request time: " << time_diff(request_start_time) << " seconds" << endl;
    setProgress(1);
    *size = output.size();
    return output;
}

//===After load, during demosaic===
//Highlight recovery parameter
void FilmImageProvider::setHighlights(int highlightsIn)
{
    QMutexLocker locker (&mutex);
    highlights = highlightsIn;
    if (valid > load)
        valid = load;
    emit highlightsChanged();
}

//===After demosaic, during prefilmulation===
//Exposure compensation of simulated film exposure
void FilmImageProvider::setExposureComp(float exposure)
{
    QMutexLocker locker(&mutex);
    exposureComp = exposure;
    if (valid > demosaic)
        valid = demosaic;
    emit exposureCompChanged();
}

//===After prefilmulation, during filmulation===
//Sets the simulated duration of film development.
void FilmImageProvider::setDevelTime(float develTimeIn)
{
    QMutexLocker locker (&mutex);
    develTime = develTimeIn;
    if (valid > prefilmulation)
        valid = prefilmulation;
    emit develTimeChanged();
}

//Sets the number of times the tank is agitated
void FilmImageProvider::setAgitateCount(int agitateCountIn)
{
    QMutexLocker locker (&mutex);
    agitateCount = agitateCountIn;
    if (valid > prefilmulation)
        valid = prefilmulation;
    emit agitateCountChanged();
}

//Sets the number of simulation steps for development
void FilmImageProvider::setDevelSteps(int develStepsIn)
{
    QMutexLocker locker (&mutex);
    develSteps = develStepsIn;
    if (valid > prefilmulation)
        valid = prefilmulation;
    emit develStepsChanged();
}

//Sets the area of the film being simulated by filmulator
void FilmImageProvider::setFilmArea(float filmAreaIn)
{
    QMutexLocker locker (&mutex);
    filmArea = filmAreaIn;
    if (valid > prefilmulation)
        valid = prefilmulation;
    emit filmAreaChanged();
}

//Sets the amount of developer mixing between the bulk
// developer reservoir and the active layer against the
// film.
void FilmImageProvider::setLayerMixConst(float layerMixConstIn)
{
    QMutexLocker locker (&mutex);
    layerMixConst = layerMixConstIn;
    if (valid > prefilmulation)
        valid = prefilmulation;
    emit layerMixConstChanged();
}

//===After filmulation, during whiteblack===
//Sets the black clipping point right after filmulation
void FilmImageProvider::setBlackpoint(float blackpointIn)
{
    QMutexLocker locker (&mutex);
    blackpoint = blackpointIn;
    if (valid > filmulation)
        valid = filmulation;
    emit blackpointChanged();
}

//Sets the white clipping point right after filmulation
void FilmImageProvider::setWhitepoint(float whitepointIn)
{
    QMutexLocker locker (&mutex);
    whitepoint = whitepointIn;
    if (valid > filmulation)
        valid = filmulation;
    emit whitepointChanged();
}

//Sets whether or not to use a default tone curve after clipping
void FilmImageProvider::setDefaultToneCurveEnabled(bool enabledIn)
{
    QMutexLocker locker (&mutex);
    defaultToneCurveEnabled = enabledIn;
    if (valid > filmulation)
        valid = filmulation;
    emit defaultToneCurveEnabledChanged();
}

//===After whiteblack, during colorcurve===

//===After colorcurve, during filmlikecurve===
//Y value of shadow control curve point
void FilmImageProvider::setShadowsY(float shadowsYIn)
{
    QMutexLocker locker (&mutex);
    shadowsY = shadowsYIn;
    if (valid > colorcurve)
        valid = colorcurve;
    emit shadowsYChanged();
}

//Y value of highlight control curve point
void FilmImageProvider::setHighlightsY(float highlightsYIn)
{
    QMutexLocker locker (&mutex);
    highlightsY = highlightsYIn;
    if (valid > colorcurve)
        valid = colorcurve;
    emit highlightsYChanged();
}

void FilmImageProvider::setProgress(float percentDone_in)
{
    progress = percentDone_in;
    emit progressChanged();
}

void FilmImageProvider::updateProgress(float percentDone_in)//Percent filmulation
{
    progress = 0.2 + percentDone_in*0.6;
    emit progressChanged();
}

void FilmImageProvider::invalidateImage()
{
    QMutexLocker locker(&mutex);
    valid = none;
}

float FilmImageProvider::getHistogramPoint(histogram &hist, int index, int i, LogY isLog)
{
    //index is 0 for L, 1 for R, 2 for G, and 3 for B.
    assert((index < 4) && (index >= 0));
    switch (index)
    {
    case 0: //luminance
        if (!isLog)
            return float(min(hist.lHist[i],hist.lHistMax))/float(hist.lHistMax);
        else
            return log(hist.lHist[i]+1)/log(hist.lHistMax+1);
    case 1: //red
        if (!isLog)
            return float(min(hist.rHist[i],hist.rHistMax))/float(hist.rHistMax);
        else
            return log(hist.rHist[i]+1)/log(hist.rHistMax+1);
    case 2: //green
        if (!isLog)
            return float(min(hist.gHist[i],hist.gHistMax))/float(hist.gHistMax);
        else
            return log(hist.gHist[i]+1)/log(hist.gHistMax+1);
    case 3: //blue
        if (!isLog)
            return float(min(hist.bHist[i],hist.bHistMax))/float(hist.bHistMax);
        else
            return log(hist.bHist[i]+1)/log(hist.bHistMax+1);
    }
    //xHistMax is the maximum height of any bin except the extremes.

    //return float(min(hist.allHist[i*4+index],hist.histMax[index]))/float(hist.histMax[index]); //maximum is the max of all elements except 0 and 127
}

QImage FilmImageProvider::emptyImage()
{
    return QImage(0,0,QImage::Format_ARGB32);
}

bool FilmImageProvider::checkAbort(Valid currStep)
{
    if (valid < currStep && time_diff(request_start_time) > 0.1)
        return true;
    else
        return false;
}

unsigned short FilmImageProvider::lookup(unsigned short in)
{
    return 65535*default_tonecurve(
                shadows_highlights(float(in)/65535.0,shadowsX,shadowsY,
                                   highlightsX,highlightsY)
                ,defaultToneCurveEnabled);
}