package com.cs.android;

/** Plain data holder returned by {@link CsUsbSerial#enumerate()}. */
public class CsPortInfo {
    public String name;
    public String description;
    public String manufacturer;
    public String serialNumber;
    public String systemLocation;
    public int vid;
    public int pid;
}
